/* Copyright (C)2004 Landmark Graphics
 * Copyright (C)2005, 2006 Sun Microsystems, Inc.
 *
 * This library is free software and may be redistributed and/or modified under
 * the terms of the wxWindows Library License, Version 3 or (at your option)
 * any later version.  The full license is in the LICENSE.txt file included
 * with this distribution.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * wxWindows Library License for more details.
 */

#include "rrdisplayclient.h"
#include "rrtimer.h"
#include "fakerconfig.h"

extern FakerConfig fconfig;

void rrdisplayclient::sendheader(rrframeheader h, bool eof=false)
{
	if(_v.major==0 && _v.minor==0)
	{
		// Fake up an old (protocol v1.0) EOF packet and see if the client sends
		// back a CTS signal.  If so, it needs protocol 1.0
		rrframeheader_v1 h1;  char reply=0;
		cvthdr_v1(h, h1);
		h1.flags=RR_EOF;
		endianize_v1(h1);
		if(_sd)
		{
			send((char *)&h1, sizeof_rrframeheader_v1);
			recv(&reply, 1);
			if(reply==1) {_v.major=1;  _v.minor=0;}
			else if(reply=='V')
			{
				rrversion v;
				_v.id[0]=reply;
				recv((char *)&_v.id[1], sizeof_rrversion-1);
				if(strncmp(_v.id, "VGL", 3) || _v.major<1)
					_throw("Error reading client version");
				v=_v;
				v.major=RR_MAJOR_VERSION;  v.minor=RR_MINOR_VERSION;
				send((char *)&v, sizeof_rrversion);
			}
			if(fconfig.verbose)
				rrout.println("[VGL] Client version: %d.%d", _v.major, _v.minor);
		}
	}
	if(eof) h.flags=RR_EOF;
	if(_v.major==1 && _v.minor==0)
	{
		rrframeheader_v1 h1;
		if(h.dpynum>255) _throw("Display number out of range for v1.0 client");
		cvthdr_v1(h, h1);
		endianize_v1(h1);
		if(_sd)
		{
			send((char *)&h1, sizeof_rrframeheader_v1);
			if(eof)
			{
				char cts=0;
				recv(&cts, 1);
				if(cts<1 || cts>2) _throw("CTS Error");
			}
		}
	}
	else
	{
		endianize(h);
		send((char *)&h, sizeof_rrframeheader);
	}
}


rrdisplayclient::rrdisplayclient(char *displayname) : _np(fconfig.np),
	_sd(NULL), _bmpi(0), _t(NULL), _deadyet(false), _dpynum(0),
	_stereo(true)
{
	memset(&_v, 0, sizeof(rrversion));
	if(fconfig.verbose)
		rrout.println("[VGL] Using %d / %d CPU's for compression",
			(int)fconfig.np, numprocs());
	char *servername=NULL;
	try
	{
		if(displayname)
		{
			char *ptr=NULL;  servername=strdup(displayname);
			if((ptr=strchr(servername, ':'))!=NULL)
			{
				if(strlen(ptr)>1) _dpynum=atoi(ptr+1);
				if(_dpynum<0 || _dpynum>65535) _dpynum=0;
				*ptr='\0';
			}
			if(!strlen(servername) || !strcmp(servername, "unix"))
				{free(servername);  servername=strdup("localhost");}
		}
		connect(servername);
		_prof_total.setname("Total     ");
		errifnot(_t=new Thread(this));
		_t->start();
	}
	catch(...)
	{
		if(servername) free(servername);  throw;
	}
	if(servername) free(servername);
}

void rrdisplayclient::run(void)
{
	rrframe *lastb=NULL, *b=NULL;
	long bytes=0;
	rrtimer t, sleept;  double err=0.;  bool first=true;
	int i;

	try {

	rrcompressor *c[MAXPROCS];  Thread *ct[MAXPROCS];
	for(i=0; i<_np; i++)
		errifnot(c[i]=new rrcompressor(i, this));
	if(_np>1) for(i=1; i<_np; i++)
	{
		errifnot(ct[i]=new Thread(c[i]));
		ct[i]->start();
	}

	while(!_deadyet)
	{
		b=NULL;
		_q.get((void **)&b);  if(_deadyet) break;
		if(!b) _throw("Queue has been shut down");
		_ready.signal();
		if(b->_h.flags==RR_RIGHT && !_stereo) continue;
		if(_np>1)
			for(i=1; i<_np; i++) {
				ct[i]->checkerror();  c[i]->go(b, lastb);
			}
		c[0]->compresssend(b, lastb);
		bytes+=c[0]->_bytes;
		if(_np>1)
			for(i=1; i<_np; i++) {
				c[i]->stop();  ct[i]->checkerror();  c[i]->send();
				bytes+=c[i]->_bytes;
			}

		sendheader(b->_h, true);

		_prof_total.endframe(b->_h.width*b->_h.height, bytes, 1);
		bytes=0;
		_prof_total.startframe();

		if(fconfig.fps>0.)
		{
			double elapsed=t.elapsed();
			if(first) first=false;
			else
			{
				if(elapsed<1./fconfig.fps)
				{
					sleept.start();
					long usec=(long)((1./fconfig.fps-elapsed-err)*1000000.);
					if(usec>0) usleep(usec);
					double sleeptime=sleept.elapsed();
					err=sleeptime-(1./fconfig.fps-elapsed-err);  if(err<0.) err=0.;
				}
			}
			t.start();
		}

		lastb=b;
	}

	for(i=0; i<_np; i++) c[i]->shutdown();
	if(_np>1) for(i=1; i<_np; i++)
	{
		ct[i]->stop();
		ct[i]->checkerror();
		delete ct[i];
	}
	for(i=0; i<_np; i++) delete c[i];

	} catch(rrerror &e)
	{
		if(_t) _t->seterror(e);
		_ready.signal();
 		throw;
	}
}

rrframe *rrdisplayclient::getbitmap(int w, int h, int ps, int flags,
	bool stereo)
{
	rrframe *b=NULL;
	_ready.wait();  if(_deadyet) return NULL;
	if(_t) _t->checkerror();
	_bmpmutex.lock();
	b=&_bmp[_bmpi];  _bmpi=(_bmpi+1)%NB;
	_bmpmutex.unlock();
	rrframeheader hdr;
	memset(&hdr, 0, sizeof(rrframeheader));
	hdr.height=hdr.frameh=h;
	hdr.width=hdr.framew=w;
	hdr.x=hdr.y=0;
	b->init(hdr, ps, flags, stereo);
	return b;
}

bool rrdisplayclient::frameready(void)
{
	if(_t) _t->checkerror();
	return(_q.items()<=0);
}

void rrdisplayclient::sendframe(rrframe *b)
{
	if(_t) _t->checkerror();
	b->_h.dpynum=_dpynum;
	_q.add((void *)b);
}

void rrdisplayclient::sendcompressedframe(rrframeheader &h, unsigned char *bits)
{
	h.dpynum=_dpynum;
	sendheader(h);
	send((char *)bits, h.size);
	sendheader(h, true);
}

void rrcompressor::compresssend(rrframe *b, rrframe *lastb)
{
	bool bu=false;  rrjpeg jpg;
	if(!b) return;
	if(b->_flags&RRBMP_BOTTOMUP) bu=true;
	int tilesizex=fconfig.tilesize? fconfig.tilesize:b->_h.height;
	int tilesizey=fconfig.tilesize? fconfig.tilesize:b->_h.width;
	int i, j, n=0;

	_bytes=0;
	for(i=0; i<b->_h.height; i+=tilesizey)
	{
		int h=tilesizey, y=i;
		if(b->_h.height-i<(3*tilesizey/2)) {h=b->_h.height-i;  i+=tilesizey;}
		for(j=0; j<b->_h.width; j+=tilesizex, n++)
		{
			int w=tilesizex, x=j;
			if(b->_h.width-j<(3*tilesizex/2)) {w=b->_h.width-j;  j+=tilesizex;}
			if(n%_np!=_myrank) continue;
			if(b->tileequals(lastb, x, y, w, h)) continue;
			rrframe *rrb=b->gettile(x, y, w, h);
			rrjpeg *jptr=NULL;
			if(_myrank>0) {errifnot(jptr=new rrjpeg());}
			else jptr=&jpg;
			_prof_comp.startframe();
			*jptr=*rrb;
			double frames=(double)(rrb->_h.width*rrb->_h.height)
				/(double)(rrb->_h.framew*rrb->_h.frameh);
			_prof_comp.endframe(rrb->_h.width*rrb->_h.height, 0, frames);
			_bytes+=jptr->_h.size;  if(jptr->_stereo) _bytes+=jptr->_rh.size;
			delete rrb;
			if(_myrank==0)
			{
				_parent->sendheader(jptr->_h);
				_parent->send((char *)jptr->_bits, jptr->_h.size);
				if(jptr->_stereo && jptr->_rbits)
				{
					_parent->sendheader(jptr->_rh);
					_parent->send((char *)jptr->_rbits, jptr->_rh.size);
				}
			}
			else
			{	
				store(jptr);
			}
		}
	}
}

void rrdisplayclient::send(char *buf, int len)
{
	try
	{
		if(_sd) _sd->send(buf, len);
	}
	catch(...)
	{
		rrout.println("[VGL] Error sending data to client.  Client may have disconnected.");
		throw;
	}
}

void rrdisplayclient::recv(char *buf, int len)
{
	try
	{
		if(_sd) _sd->recv(buf, len);
	}
	catch(...)
	{
		rrout.println("[VGL] Error receiving data from client.  Client may have disconnected.");
		throw;
	}
}

void rrdisplayclient::connect(char *servername)
{
	if(servername)
	{
		errifnot(_sd=new rrsocket(fconfig.ssl));
		try
		{
			_sd->connect(servername, fconfig.port);
		}
		catch(...)
		{
			rrout.println("[VGL] Could not connect to VGL client.  Make sure the VGL client is running and");
			rrout.println("[VGL]    that either the DISPLAY or VGL_CLIENT environment variable points to");
			rrout.println("[VGL]    the machine on which it is running.");
			throw;
		}
	}
}

void rrcompressor::send(void)
{
	for(int i=0; i<_storedframes; i++)
	{
		rrjpeg *j=_frame[i];
		errifnot(j);
		_parent->sendheader(j->_h);
		_parent->send((char *)j->_bits, j->_h.size);
		if(j->_stereo && j->_rbits)
		{
			_parent->sendheader(j->_rh);
			_parent->send((char *)j->_rbits, j->_rh.size);
		}
		delete j;		
	}
	_storedframes=0;
}
