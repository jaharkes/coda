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





#ifdef __cplusplus
extern "C" {
#endif __cplusplus

#include <sys/types.h>
#include <errno.h>

#include <lwp/lwp.h>
#include <lwp/lock.h>
#include <rpc2/rpc2.h>
#include <util.h>
#include <rvmlib.h>

#include <volutil.h>
#ifdef __cplusplus
}
#endif __cplusplus

#include <voltypes.h>
#include <vice.h>
#include <cvnode.h>
#include <volume.h>
#include <camprivate.h>
#include <vutil.h>
#include "vvlist.h"

/*
  BEGIN_HTML
  <a name="S_VolMarkAsAncient"><strong>Mark the older dump file of a volume as ancient</strong></a> 
  END_HTML
*/
long S_VolMarkAsAncient(RPC2_Handle rpcid, VolumeId groupId, VolumeId repId)
{
    ProgramType *pt;
    int rc = 0;
    char *errstr;
    
    CODA_ASSERT(LWP_GetRock(FSTAG, (char **)&pt) == LWP_SUCCESS);

    LogMsg(9, VolDebugLevel, stdout, "Entering S_VolMarkAsAncient: rpcid = %d, groupId = %x, repId = %x",
	rpcid, groupId, repId);

    rc = VInitVolUtil(volumeUtility);
    if (rc != 0)
	return rc;

    char listfile[MAXLISTNAME];
    getlistfilename(listfile, groupId, repId, "newlist");

    char newlistfile[MAXLISTNAME];
    getlistfilename(newlistfile, groupId, repId, "ancient");

    if (rename(listfile, newlistfile) < 0) {
	errstr = strerror(errno);
	LogMsg(0, VolDebugLevel, stdout,
	       "MarkAsAncient: rename %s->%s failed, %s",
	       listfile, newlistfile, errstr != NULL ? errstr: "Cannot rename");
	VDisconnectFS();
	return VFAIL;
    }

    VDisconnectFS();
    LogMsg(0, VolDebugLevel, stdout, "MarkAsAncient succeeded");

    return RPC2_SUCCESS;
}
