/* BLURB gpl

                           Coda File System
                              Release 5

          Copyright (c) 1987-1999 Carnegie Mellon University
                  Additional copyrights listed below

This  code  is  distributed "AS IS" without warranty of any kind under
the terms of the GNU General Public Licence Version 2, as shown in the
file  LICENSE.  The  technical and financial  contributors to Coda are
listed in the file CREDITS.

                        Additional copyrights
                           none currently

#*/




/*
 *  Code relating to volume callbacks.
 */

#ifdef __cplusplus
extern "C" {
#endif __cplusplus

#include <stdio.h>
#include <sys/types.h>
#include <struct.h>
#include <unistd.h>
#include <stdlib.h>
#include <netinet/in.h>
#ifdef __BSD44__
#include <machine/endian.h>
#endif

#include <rpc2/rpc2.h>

#ifdef __cplusplus
}
#endif __cplusplus

/* interfaces */
#include <vice.h>
#include <writeback.h>
#include <mond.h>

/* from venus */
#include "comm.h"
#include "fso.h"
#include "mariner.h"
#include "venuscb.h"
#include "venusvol.h"
#include "venus.private.h"
#include "vproc.h"
#include "worker.h"

void volent::SetPermit()
{
    VPStatus = PermitSet;
    LOG(1, ("volent::SetPermit(): hey, i just set a permit!\n"));
}

/* ok, so this makes an RPC call to the server to try to get a permit;
   we return PermitSet if we successfully got one, otherwise NoPermit */
int volent::GetPermit()
{
    long permit;

    connent *c;
    int code = GetAdmConn(&c);
    ViceFid fid;
    fid.Volume = vid;
    fid.Vnode = 0;
    fid.Unique = 0;
    if (code != 0)
	return code;

    ViceGetWBPermit(c->connid, vid, &fid, &permit);

    LOG(1, ("volent::GetPermit(): ViceGetWBPermit replied with %d\n", permit));

    switch (permit) {
    case WB_PERMIT_GRANTED:
	SetPermit();
	break;
    case WB_LOOKUPFAILED:
    case WB_OTHERCLIENT:
    case WB_DISABLED:
	ClearPermit();
	break;
    }

    return VPStatus;
}

void volent::ClearPermit()
{
    VPStatus = NoPermit;

    LOG(1, ("volent::ClearPermit(): hey, I just cleared a permit!\n"));
    
}

int volent::PermitBreak()
{
    VPStatus = NoPermit;

    Reintegrate();
}
