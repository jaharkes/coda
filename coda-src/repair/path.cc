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
   Routines pertaining to pathname processing for repair tool.
   NONE of these routines have any global side effects.

   Created: M. Satyanarayanan
            October 1989
*/


#ifdef __cplusplus
extern "C" {
#endif __cplusplus

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <errno.h>
#include "coda_assert.h"
#include <setjmp.h>
#include <sys/types.h>
#include <sys/param.h>
#include <netinet/in.h>
#include "coda_string.h"
#include <sys/stat.h>
#include <rpc2/rpc2.h>
#include <unistd.h>
#include <stdlib.h>

#include <inodeops.h>
#ifdef __cplusplus
}
#endif __cplusplus

#include <vice.h>
#include <venusioctl.h>
#include <repio.h>
#include "repair.h"

extern int session;

static char *repair_abspath(char *result, unsigned int len, char *name);
static int repair_getvid(char *, VolumeId *);

/*
 leftmost: check pathname for inconsistent object

 path:	user-provided path of alleged object in conflict
 realpath:	true path (sans sym links) of object in conflict
 
 Returns 0 iff path refers to an object in conflict and this is the
           leftmost such object on its true path (as returned by getwd())
 Returns -1, after printing error messages, in all other cases.  
*/
int repair_isleftmost(char *path, char *realpath, int len)
{
    register char *car, *cdr;
    int symlinks;
    char buf[MAXPATHLEN], symbuf[MAXPATHLEN], here[MAXPATHLEN], tmp[MAXPATHLEN];
    
    DEBUG(("repair_isleftmost(%s, %s)\n", path, realpath));
    strcpy(buf, path); /* tentative */
    symlinks = 0;
    if (!getcwd(here, sizeof(here))) { /* remember where we are */
	printf("Couldn't stat current working directory\n");
	exit(-1);
    }
#define RETURN(x) {CODA_ASSERT(!chdir(here)); return(x);}

    /* simulate namei() */
    while (1)
	{
	/* start at beginning of buf */
	if (*buf == '/')
	    {
	    CODA_ASSERT(!chdir("/"));
	    car = buf+1;
	    }
	else car = buf;
	
	/* proceed left to right */
	while (1)
	    {
	    /* Lop off next piece */
	    cdr = index(car, '/');
	    if (!cdr)
	    	{/* We're at the end */
		    if (session) {
			repair_abspath(realpath, len, car);
			RETURN(0);
		    } else {
			if (repair_inconflict(car, 0) == 0)
			  {
			      repair_abspath(realpath, len, car);
			      RETURN(0);
			  } else {
			      printf("object not in conflict\n");
			      RETURN(-1);
			  }
		    }
		}
	    *cdr = 0; /* clobber slash */
	    cdr++;

	    /* Is this piece ok? */
	    if (repair_inconflict(car, 0) == 0) {
		printf("%s is to the left of %s and is in conflict\n", 
		       repair_abspath(tmp, MAXPATHLEN, car), path);
		RETURN(-1);
	    }
	    
	    /* Is this piece a sym link? */
	    if (readlink(car, symbuf, MAXPATHLEN) > 0) {
		if (++symlinks >= CODA_MAXSYMLINKS)
		    {
		    errno = ELOOP;
		    perror(path);
		    RETURN(-1);
		    }
		strcat(symbuf, "/");
		strcat(symbuf, cdr);
		strcpy(buf, symbuf);
		break; /* to outer loop, and restart scan */
		}
		
	    /* cd to next component */
	    if (chdir(car) < 0)
		{
		perror(repair_abspath(tmp, MAXPATHLEN, car));
		RETURN(-1);
		}
		
	    /* Phew! Traversed another component! */
	    car = cdr;
	    *(cdr-1) = '/'; /* Restore clobbered slash */
	    }
	}
#undef RETURN
    }
    


/*
    Obtains mount point of last volume in realpath
    Returns 0 on success, -1 on failure.
    Null OUT parameters will not be filled.
       
    realpath:	abs pathname (sans sym links)of replicated object
    prefix:	part before last volume encountered in realpath
    suffix:	part inside last volume
    vid:	id of last volume

    CAVEAT: code assumes realpath has no conflicts except (possibly)
          last component.  This is NOT checked.
*/
int repair_getmnt(char *realpath, char *prefix, char *suffix, VolumeId *vid)
{
    char buf[MAXPATHLEN];
    VolumeId currvid, oldvid;
    char *slash;
    char *tail; 
    int rc;

    DEBUG(("repair_getmnt(%s...)\n", realpath));

    /* Find abs path */
    CODA_ASSERT(*realpath == '/');
    strcpy(buf, realpath);
    
    /* obtain volume id of last component */
    rc = repair_getvid(buf, &currvid);
    if (rc < 0) 
	    return(-1);
    

    /* Work backwards, shrinking realpath and testing if we have
       crossed a mount point -- invariant:
        - when leaving slash points to charactar before the suffix,
	which is the relative path withoin the volume, of the
         object in conflict 
        -  will always point at starting char of suffix
    */

    tail = buf + strlen(buf); 
    slash = buf + strlen(buf); /* points at trailing null */
    oldvid = currvid;
    while (1) {

	    /* break the string and find nex right slash */
	    slash = strrchr(buf, '/'); 
	    /* abs path ==> '/' guaranteed */
	    CODA_ASSERT(slash);

	    /* possibility 1: ate whole path up */
	    if (slash == buf) {
		    break; 
	    }

	    *slash = '\0';
	    rc = repair_getvid(buf, &currvid);
	    *slash = '/';  /* restore the nuked component */

	    /* possibility 2: crossed out of Coda */
	    if (rc < 0) {
		    /* not in Coda probably */
		    if (errno == EINVAL) {
			    break; 
		    }
		    /* this is an unacceptable error */
		    perror("repair_getvid");
		    return -1;
	    }

	    /* possibility 3: crossed an internal Coda mount point */
	    if (oldvid != currvid) {
		    /* restore slash to previous value and break */
		    break;
	    }

	    /* possibility 4: we are still in the same volume */
	    *slash = '\0';
	    /* restore the previous null */
	    if ( *tail != '\0' )
		    *(tail - 1) = '/';
	    tail = slash + 1;
    }

    /* set OUT parameters */
    if (prefix) 
	    strcpy(prefix, buf);  /* this gives us the mount point */
    if (vid) 
	    *vid = oldvid;
    if (suffix )
	    strcpy(suffix, tail); 

    return(0);
}

/* 
   returns 0 if name refers to an object in conflict
   and conflictfid is filled  if it is non-null.
   returns -1 otherwise (even if any other error)
       
   CAVEAT: assumes no conflicts to left of last component.
   This is NOT checked.
*/
int repair_inconflict(char *name, ViceFid *conflictfid /* OUT */)
{
    int rc;
    char symval[MAXPATHLEN];
    struct stat statbuf;

    DEBUG(("repair_inconflict(%s,...)\n", name));

    rc = stat(name, &statbuf);
    if ((rc == 0) || (errno != ENOENT)) 
	    return(-1);
    
    /* is it a sym link? */
    symval[0] = 0;
    rc = readlink(name, symval, MAXPATHLEN);
    if (rc < 0) return(-1);

    /* it's a sym link, alright */
    if (symval[0] == '@')
	{
	if (conflictfid)
		sscanf(symval, "@%lx.%lx.%lx",
		       &conflictfid->Volume, &conflictfid->Vnode, 
		       &conflictfid->Unique);
	return (0);
	}
    else 
	    return(-1);
    }

/* Returns 0 and fills outfid and outvv with fid and version vector
   for specified Coda path.  If version vector is not accessible, the
   StoreId fields of outvv are set to -1.  Garbage may be copied into
   outvv for non-replicated files
	  
   Returns -1 after printing error msg on failures. 
*/
int repair_getfid(char *path, ViceFid *outfid /* OUT */,
		  ViceVersionVector *outvv /* OUT */)
{
    int rc, saveerrno;
    struct ViceIoctl vi;
    char junk[2048];

    DEBUG(("repair_getfid(%s, ...)\n", path));
    vi.in = 0;
    vi.in_size = 0;
    vi.out = junk;
    vi.out_size = sizeof(junk);
    bzero(junk, sizeof(junk));

    rc = pioctl(path, VIOC_GETFID, &vi, 0);
    saveerrno = errno;

    /* Easy: no conflicts */
    if (!rc) {
	bcopy((const char *)junk, (void *)outfid, sizeof(ViceFid));
	bcopy((const char *)junk+sizeof(ViceFid), (void *)outvv, sizeof(ViceVersionVector));
	return(0);
    }

    /* Perhaps the object is in conflict? Get fid from dangling symlink */
    rc = repair_inconflict(path, outfid);
    if (!rc) {
	outvv->StoreId.Host = (u_long) -1;  /* indicates VV is undefined */
	outvv->StoreId.Uniquifier = (u_long)-1;
	return(0);
    }

    /* No: 'twas some other bogosity */
    if (errno != EINVAL)
	repair_perror(" GETFID", path, saveerrno);
    return(-1);
}


/* return 1 when in coda */
int repair_IsInCoda(char *name) 
{
    static char buf[MAXPATHLEN];
    char *tmpname = name;
    if (name[0] != '/') {
       	tmpname = getcwd(buf, MAXPATHLEN);
	CODA_ASSERT(tmpname);
    }
    /* test absolute path name */
    if (strncmp(tmpname, "/coda", 5) == 0 ) 
	    return(1);
    else 
	    return (0);
}

static char *repair_abspath(char *result, unsigned int len, char *name)
{
    CODA_ASSERT(getcwd(result, len));
    CODA_ASSERT( strlen(name) + 1 <= len );

    strcat(result, "/");
    strcat(result, name);
    return(result);
}


/* Returns 0 and fills volid with the volume id of path.  Returns -1
   on failure */

static int repair_getvid(char *path, VolumeId *vid)
{
    ViceFid vfid;
    ViceVersionVector vv;
    int rc;

    DEBUG(("repair_getvid(%s,...)\n", path));

    rc = repair_getfid(path, &vfid, &vv);
    if (rc < 0) return(-1); /* getfid does perror() */
    *vid = vfid.Volume;
    return(0);    
    }

void repair_perror(char *op, char *path, int e)
{
    char msg[MAXPATHLEN+100];
    
    sprintf(msg, "%s: %s", op, path);
    errno = e;  /* in case it has been clobbered by now */
    perror(msg);
}
