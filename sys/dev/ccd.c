/*	$OpenBSD: ccd.c,v 1.54 2004/01/09 21:32:23 brad Exp $	*/
/*	$NetBSD: ccd.c,v 1.33 1996/05/05 04:21:14 thorpej Exp $	*/

/*-
 * Copyright (c) 1996 The NetBSD Foundation, Inc.
 * Copyright (c) 1997 Niklas Hallqvist.
 * All rights reserved.
 * 
 * This code is derived from software contributed to The NetBSD Foundation
 * by Jason R. Thorpe.
 * Niklas Hallqvist redid the buffer policy for better performance.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *        This product includes software developed by the NetBSD
 *        Foundation, Inc. and its contributors.
 * 4. Neither the name of The NetBSD Foundation nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE NETBSD FOUNDATION, INC. AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * Copyright (c) 1988 University of Utah.
 * Copyright (c) 1990, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * the Systems Programming Group of the University of Utah Computer
 * Science Department.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * from: Utah $Hdr: cd.c 1.6 90/11/28$
 *
 *	@(#)cd.c	8.2 (Berkeley) 11/16/93
 */

/*
 * "Concatenated" disk driver.
 *
 * Dynamic configuration and disklabel support by:
 *	Jason R. Thorpe <thorpej@nas.nasa.gov>
 *	Numerical Aerodynamic Simulation Facility
 *	Mail Stop 258-6
 *	NASA Ames Research Center
 *	Moffett Field, CA 94035
 *
 * Mirroring support based on code written by Satoshi Asami
 * and Nisha Talagala.
 *
 * Buffer scatter/gather policy by Niklas Hallqvist.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/errno.h>
#include <sys/buf.h>
#include <sys/malloc.h>
#include <sys/namei.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/disklabel.h>
#include <sys/device.h>
#include <sys/disk.h>
#include <sys/syslog.h>
#include <sys/fcntl.h>
#include <sys/vnode.h>
#include <sys/conf.h>

#include <dev/ccdvar.h>

#ifdef __GNUC__
#define INLINE static __inline
#else
#define INLINE
#endif

/*
 * Overridable value telling how many kvm spaces of MAXBSIZE we need for
 * component I/O operations.
 */
#ifndef CCD_CLUSTERS
#define CCD_CLUSTERS 16
#endif

#if defined(CCDDEBUG) && !defined(DEBUG)
#define DEBUG
#endif

#ifdef DEBUG
#define CCDB_FOLLOW	0x01
#define CCDB_INIT	0x02
#define CCDB_IO		0x04
#define CCDB_LABEL	0x08
#define CCDB_VNODE	0x10
int ccddebug = 0x00;
#endif

#define	ccdunit(x)	DISKUNIT(x)

struct ccdseg {
	caddr_t		cs_sgaddr;	/* scatter/gather segment addresses */
	long		cs_sglen;	/* scatter/gather segment lengths */
};

struct ccdbuf {
	struct buf	cb_buf;		/* new I/O buf */
	struct buf	*cb_obp;	/* ptr. to original I/O buf */
	int		cb_unit;	/* target unit */
	int		cb_comp;	/* target component */
	int		cb_flags;	/* misc. flags */
#define CBF_MIRROR	0x01		/* we're for a mirror component */
#define CBF_OLD		0x02		/* use old I/O protocol */

	int		cb_sgcnt;	/* scatter/gather segment count */
	struct ccdseg	*cb_sg;		/* scatter/gather segments */
};

#define CCDLABELDEV(dev)	\
	(MAKEDISKDEV(major((dev)), ccdunit((dev)), RAW_PART))

/* called by main() at boot time */
void	ccdattach(int);

/* called by biodone() at interrupt time */
void	ccdiodone(struct buf *);
int	ccdsize(dev_t);

void	ccdstart(struct ccd_softc *, struct buf *);
void	ccdinterleave(struct ccd_softc *, int);
void	ccdintr(struct ccd_softc *, struct buf *);
int	ccdinit(struct ccddevice *, char **, struct proc *);
int	ccdlookup(char *, struct proc *p, struct vnode **);
long	ccdbuffer(struct ccd_softc *, struct buf *, daddr_t, caddr_t,
    long, struct ccdbuf **, int);
void	ccdgetdisklabel(dev_t);
void	ccdmakedisklabel(struct ccd_softc *);
int	ccdlock(struct ccd_softc *);
void	ccdunlock(struct ccd_softc *);
INLINE struct ccdbuf *getccdbuf(void);
INLINE void putccdbuf(struct ccdbuf *);

#ifdef DEBUG
void	printiinfo(struct ccdiinfo *);
#endif

/* Non-private for the benefit of libkvm. */
struct	ccd_softc *ccd_softc;
struct	ccddevice *ccddevs;
int	numccd = 0;

/* A separate map so that locking on kernel_map won't happen in interrupts */
static struct vm_map *ccdmap;

/*
 * Set when a process need some kvm.
 * XXX should we fallback to old I/O policy instead when out of ccd kvm?
 */
static int ccd_need_kvm = 0;

/*
 * Manage the ccd buffer structures.
 */
INLINE struct ccdbuf *
getccdbuf()
{
	struct ccdbuf *cbp;

	cbp = malloc(sizeof (struct ccdbuf), M_DEVBUF, M_WAITOK);
	bzero(cbp, sizeof (struct ccdbuf));
	cbp->cb_sg = malloc(sizeof (struct ccdseg) * MAXBSIZE >> PAGE_SHIFT,
	    M_DEVBUF, M_WAITOK);
	return (cbp);
}

INLINE void
putccdbuf(cbp)
	struct ccdbuf *cbp;
{
	free((caddr_t)cbp->cb_sg, M_DEVBUF);
	free((caddr_t)cbp, M_DEVBUF);
}

/*
 * Called by main() during pseudo-device attachment.  All we need
 * to do is allocate enough space for devices to be configured later.
 */
void
ccdattach(num)
	int num;
{
	if (num <= 0) {
#ifdef DIAGNOSTIC
		panic("ccdattach: count <= 0");
#endif
		return;
	}

	ccd_softc = (struct ccd_softc *)malloc(num * sizeof(struct ccd_softc),
	    M_DEVBUF, M_NOWAIT);
	ccddevs = (struct ccddevice *)malloc(num * sizeof(struct ccddevice),
	    M_DEVBUF, M_NOWAIT);
	if ((ccd_softc == NULL) || (ccddevs == NULL)) {
		printf("WARNING: no memory for concatenated disks\n");
		if (ccd_softc != NULL)
			free(ccd_softc, M_DEVBUF);
		if (ccddevs != NULL)
			free(ccddevs, M_DEVBUF);
		return;
	}
	numccd = num;
	bzero(ccd_softc, num * sizeof(struct ccd_softc));
	bzero(ccddevs, num * sizeof(struct ccddevice));
}

int
ccdinit(ccd, cpaths, p)
	struct ccddevice *ccd;
	char **cpaths;
	struct proc *p;
{
	struct ccd_softc *cs = &ccd_softc[ccd->ccd_unit];
	struct ccdcinfo *ci = NULL;
	size_t size;
	int ix;
	struct vnode *vp;
	struct vattr va;
	size_t minsize;
	int maxsecsize;
	struct partinfo dpart;
	struct ccdgeom *ccg = &cs->sc_geom;
	char tmppath[MAXPATHLEN];
	int error;

#ifdef DEBUG
	if (ccddebug & (CCDB_FOLLOW|CCDB_INIT))
		printf("ccdinit: unit %d\n", ccd->ccd_unit);
#endif

	cs->sc_size = 0;
	cs->sc_ileave = ccd->ccd_interleave;
	cs->sc_nccdisks = ccd->ccd_ndev;
	if (snprintf(cs->sc_xname, sizeof(cs->sc_xname), "ccd%d",
	    ccd->ccd_unit) >= sizeof(cs->sc_xname)) {
		printf("ccdinit: device name too long.\n");
		return(ENXIO);
	}

	/* Allocate space for the component info. */
	cs->sc_cinfo = malloc(cs->sc_nccdisks * sizeof(struct ccdcinfo),
	    M_DEVBUF, M_WAITOK);

	/*
	 * Verify that each component piece exists and record
	 * relevant information about it.
	 */
	maxsecsize = 0;
	minsize = 0;
	for (ix = 0; ix < cs->sc_nccdisks; ix++) {
		vp = ccd->ccd_vpp[ix];
		ci = &cs->sc_cinfo[ix];
		ci->ci_vp = vp;

		/*
		 * Copy in the pathname of the component.
		 */
		bzero(tmppath, sizeof(tmppath));	/* sanity */
		error = copyinstr(cpaths[ix], tmppath,
		    MAXPATHLEN, &ci->ci_pathlen);
		if (error) {
#ifdef DEBUG
			if (ccddebug & (CCDB_FOLLOW|CCDB_INIT))
				printf("%s: can't copy path, error = %d\n",
				    cs->sc_xname, error);
#endif
			free(cs->sc_cinfo, M_DEVBUF);
			return (error);
		}
		ci->ci_path = malloc(ci->ci_pathlen, M_DEVBUF, M_WAITOK);
		bcopy(tmppath, ci->ci_path, ci->ci_pathlen);

		/*
		 * XXX: Cache the component's dev_t.
		 */
		if ((error = VOP_GETATTR(vp, &va, p->p_ucred, p)) != 0) {
#ifdef DEBUG
			if (ccddebug & (CCDB_FOLLOW|CCDB_INIT))
				printf("%s: %s: getattr failed %s = %d\n",
				    cs->sc_xname, ci->ci_path,
				    "error", error);
#endif
			free(ci->ci_path, M_DEVBUF);
			free(cs->sc_cinfo, M_DEVBUF);
			return (error);
		}
		ci->ci_dev = va.va_rdev;

		/*
		 * Get partition information for the component.
		 */
		error = VOP_IOCTL(vp, DIOCGPART, (caddr_t)&dpart,
		    FREAD, p->p_ucred, p);
		if (error) {
#ifdef DEBUG
			if (ccddebug & (CCDB_FOLLOW|CCDB_INIT))
				 printf("%s: %s: ioctl failed, error = %d\n",
				     cs->sc_xname, ci->ci_path, error);
#endif
			free(ci->ci_path, M_DEVBUF);
			free(cs->sc_cinfo, M_DEVBUF);
			return (error);
		}
		if (dpart.part->p_fstype == FS_CCD ||
		    dpart.part->p_fstype == FS_BSDFFS) {
			maxsecsize =
			    ((dpart.disklab->d_secsize > maxsecsize) ?
			    dpart.disklab->d_secsize : maxsecsize);
			size = dpart.part->p_size;
		} else {
#ifdef DEBUG
			if (ccddebug & (CCDB_FOLLOW|CCDB_INIT))
				printf("%s: %s: incorrect partition type\n",
				    cs->sc_xname, ci->ci_path);
#endif
			free(ci->ci_path, M_DEVBUF);
			free(cs->sc_cinfo, M_DEVBUF);
			return (EFTYPE);
		}

		/*
		 * Calculate the size, truncating to an interleave
		 * boundary if necessary.
		 */
		if (cs->sc_ileave > 1)
			size -= size % cs->sc_ileave;

		if (size == 0) {
#ifdef DEBUG
			if (ccddebug & (CCDB_FOLLOW|CCDB_INIT))
				printf("%s: %s: size == 0\n",
				    cs->sc_xname, ci->ci_path);
#endif
			free(ci->ci_path, M_DEVBUF);
			free(cs->sc_cinfo, M_DEVBUF);
			return (ENODEV);
		}

		if (minsize == 0 || size < minsize)
			minsize = size;
		ci->ci_size = size;
		cs->sc_size += size;
	}

	/*
	 * Don't allow the interleave to be smaller than
	 * the biggest component sector.
	 */
	if ((cs->sc_ileave > 0) &&
	    (cs->sc_ileave < (maxsecsize / DEV_BSIZE))) {
#ifdef DEBUG
		if (ccddebug & (CCDB_FOLLOW|CCDB_INIT))
			printf("%s: interleave must be at least %d\n",
			    cs->sc_xname, (maxsecsize / DEV_BSIZE));
#endif
		free(ci->ci_path, M_DEVBUF);
		free(cs->sc_cinfo, M_DEVBUF);
		return (EINVAL);
	}

	/*
	 * Mirroring support requires uniform interleave and
	 * and even number of components.
	 */
	if (ccd->ccd_flags & CCDF_MIRROR) {
		ccd->ccd_flags |= CCDF_UNIFORM;
		if (cs->sc_ileave == 0) {
#ifdef DEBUG
			if (ccddebug & (CCDB_FOLLOW|CCDB_INIT))
			printf("%s: mirroring requires interleave\n",
			    cs->sc_xname);
#endif
			free(ci->ci_path, M_DEVBUF);
			free(cs->sc_cinfo, M_DEVBUF);
			return (EINVAL);
		}
		if (cs->sc_nccdisks % 2) { 
#ifdef DEBUG
			if (ccddebug & (CCDB_FOLLOW|CCDB_INIT))
			printf("%s: mirroring requires even # of components\n",
			    cs->sc_xname); 
#endif
			free(ci->ci_path, M_DEVBUF);
			free(cs->sc_cinfo, M_DEVBUF);
			return (EINVAL);
		}
	}

	/*
	 * If uniform interleave is desired set all sizes to that of
	 * the smallest component.
	 */
	if (ccd->ccd_flags & CCDF_UNIFORM) {
		for (ci = cs->sc_cinfo;
		     ci < &cs->sc_cinfo[cs->sc_nccdisks]; ci++)
			ci->ci_size = minsize;

		if (ccd->ccd_flags & CCDF_MIRROR)
			cs->sc_size = (cs->sc_nccdisks / 2) * minsize;
		else
			cs->sc_size = cs->sc_nccdisks * minsize;
	}

	/*
	 * Construct the interleave table.
	 */
	ccdinterleave(cs, ccd->ccd_unit);

	/*
	 * Create pseudo-geometry based on 1MB cylinders.  It's
	 * pretty close.
	 */
	ccg->ccg_secsize = DEV_BSIZE;
	ccg->ccg_ntracks = 1;
	ccg->ccg_nsectors = 1024 * (1024 / ccg->ccg_secsize);
	ccg->ccg_ncylinders = cs->sc_size / ccg->ccg_nsectors;

	cs->sc_flags |= CCDF_INITED;
	cs->sc_cflags = ccd->ccd_flags;	/* So we can find out later... */
	cs->sc_unit = ccd->ccd_unit;

	return (0);
}

void
ccdinterleave(cs, unit)
	struct ccd_softc *cs;
	int unit;
{
	struct ccdcinfo *ci, *smallci;
	struct ccdiinfo *ii;
	daddr_t bn, lbn;
	int ix;
	u_long size;

#ifdef DEBUG
	if (ccddebug & CCDB_INIT)
		printf("ccdinterleave(%p): ileave %d\n", cs, cs->sc_ileave);
#endif
	/*
	 * Allocate an interleave table.
	 * Chances are this is too big, but we don't care.
	 */
	size = (cs->sc_nccdisks + 1) * sizeof(struct ccdiinfo);
	cs->sc_itable = (struct ccdiinfo *)malloc(size, M_DEVBUF, M_WAITOK);
	bzero((caddr_t)cs->sc_itable, size);

	/*
	 * Trivial case: no interleave (actually interleave of disk size).
	 * Each table entry represents a single component in its entirety.
	 */
	if (cs->sc_ileave == 0) {
		bn = 0;
		ii = cs->sc_itable;

		for (ix = 0; ix < cs->sc_nccdisks; ix++) {
			/* Allocate space for ii_index. */
			ii->ii_index = malloc(sizeof(int), M_DEVBUF, M_WAITOK);
			ii->ii_ndisk = 1;
			ii->ii_startblk = bn;
			ii->ii_startoff = 0;
			ii->ii_index[0] = ix;
			bn += cs->sc_cinfo[ix].ci_size;
			ii++;
		}
		ii->ii_ndisk = 0;
#ifdef DEBUG
		if (ccddebug & CCDB_INIT)
			printiinfo(cs->sc_itable);
#endif
		return;
	}

	/*
	 * The following isn't fast or pretty; it doesn't have to be.
	 */
	size = 0;
	bn = lbn = 0;
	for (ii = cs->sc_itable; ; ii++) {
		/* Allocate space for ii_index. */
		ii->ii_index = malloc((sizeof(int) * cs->sc_nccdisks),
		    M_DEVBUF, M_WAITOK);

		/*
		 * Locate the smallest of the remaining components
		 */
		smallci = NULL;
		for (ci = cs->sc_cinfo;
		    ci < &cs->sc_cinfo[cs->sc_nccdisks]; ci++)
			if (ci->ci_size > size &&
			    (smallci == NULL ||
			    ci->ci_size < smallci->ci_size))
				smallci = ci;

		/*
		 * Nobody left, all done
		 */
		if (smallci == NULL) {
			ii->ii_ndisk = 0;
			break;
		}

		/*
		 * Record starting logical block and component offset
		 */
		ii->ii_startblk = bn / cs->sc_ileave;
		ii->ii_startoff = lbn;

		/*
		 * Determine how many disks take part in this interleave
		 * and record their indices.
		 */
		ix = 0;
		for (ci = cs->sc_cinfo;
		    ci < &cs->sc_cinfo[cs->sc_nccdisks]; ci++)
			if (ci->ci_size >= smallci->ci_size)
				ii->ii_index[ix++] = ci - cs->sc_cinfo;
		ii->ii_ndisk = ix;
		bn += ix * (smallci->ci_size - size);
		lbn = smallci->ci_size / cs->sc_ileave;
		size = smallci->ci_size;
	}
#ifdef DEBUG
	if (ccddebug & CCDB_INIT)
		printiinfo(cs->sc_itable);
#endif
}

/* ARGSUSED */
int
ccdopen(dev, flags, fmt, p)
	dev_t dev;
	int flags, fmt;
	struct proc *p;
{
	int unit = ccdunit(dev);
	struct ccd_softc *cs;
	struct disklabel *lp;
	int error = 0, part, pmask;

#ifdef DEBUG
	if (ccddebug & CCDB_FOLLOW)
		printf("ccdopen(%x, %x)\n", dev, flags);
#endif
	if (unit >= numccd)
		return (ENXIO);
	cs = &ccd_softc[unit];

	if ((error = ccdlock(cs)) != 0)
		return (error);

	lp = cs->sc_dkdev.dk_label;

	part = DISKPART(dev);
	pmask = (1 << part);

	/*
	 * If we're initialized, check to see if there are any other
	 * open partitions.  If not, then it's safe to update
	 * the in-core disklabel.
	 */
	if ((cs->sc_flags & CCDF_INITED) && (cs->sc_dkdev.dk_openmask == 0))
		ccdgetdisklabel(dev);

	/* Check that the partition exists. */
	if (part != RAW_PART) {
		if (((cs->sc_flags & CCDF_INITED) == 0) ||
		    ((part >= lp->d_npartitions) ||
		    (lp->d_partitions[part].p_fstype == FS_UNUSED))) {
			error = ENXIO;
			goto done;
		}
	}

	/* Prevent our unit from being unconfigured while open. */
	switch (fmt) {
	case S_IFCHR:
		cs->sc_dkdev.dk_copenmask |= pmask;
		break;

	case S_IFBLK:
		cs->sc_dkdev.dk_bopenmask |= pmask;
		break;
	}
	cs->sc_dkdev.dk_openmask =
	    cs->sc_dkdev.dk_copenmask | cs->sc_dkdev.dk_bopenmask;

 done:
	ccdunlock(cs);
	return (error);
}

/* ARGSUSED */
int
ccdclose(dev, flags, fmt, p)
	dev_t dev;
	int flags, fmt;
	struct proc *p;
{
	int unit = ccdunit(dev);
	struct ccd_softc *cs;
	int error = 0, part;

#ifdef DEBUG
	if (ccddebug & CCDB_FOLLOW)
		printf("ccdclose(%x, %x)\n", dev, flags);
#endif

	if (unit >= numccd)
		return (ENXIO);
	cs = &ccd_softc[unit];

	if ((error = ccdlock(cs)) != 0)
		return (error);

	part = DISKPART(dev);

	/* ...that much closer to allowing unconfiguration... */
	switch (fmt) {
	case S_IFCHR:
		cs->sc_dkdev.dk_copenmask &= ~(1 << part);
		break;

	case S_IFBLK:
		cs->sc_dkdev.dk_bopenmask &= ~(1 << part);
		break;
	}
	cs->sc_dkdev.dk_openmask =
	    cs->sc_dkdev.dk_copenmask | cs->sc_dkdev.dk_bopenmask;

	ccdunlock(cs);
	return (0);
}

void
ccdstrategy(bp)
	struct buf *bp;
{
	int unit = ccdunit(bp->b_dev);
	struct ccd_softc *cs = &ccd_softc[unit];
	int s;
	int wlabel;
	struct disklabel *lp;

#ifdef DEBUG
	if (ccddebug & CCDB_FOLLOW)
		printf("ccdstrategy(%p): unit %d\n", bp, unit);
#endif
	if ((cs->sc_flags & CCDF_INITED) == 0) {
		bp->b_error = ENXIO;
		bp->b_resid = bp->b_bcount;
		bp->b_flags |= B_ERROR;
		goto done;
	}

	/* If it's a nil transfer, wake up the top half now. */
	if (bp->b_bcount == 0)
		goto done;

	lp = cs->sc_dkdev.dk_label;

	/*
	 * Do bounds checking and adjust transfer.  If there's an
	 * error, the bounds check will flag that for us.
	 */
	wlabel = cs->sc_flags & (CCDF_WLABEL|CCDF_LABELLING);
	if (DISKPART(bp->b_dev) != RAW_PART &&
	    bounds_check_with_label(bp, lp, cs->sc_dkdev.dk_cpulabel,
	    wlabel) <= 0)
		goto done;

	bp->b_resid = bp->b_bcount;

	/*
	 * "Start" the unit.
	 */
	s = splbio();
	ccdstart(cs, bp);
	splx(s);
	return;
done:
	s = splbio();
	biodone(bp);
	splx(s);
}

void
ccdstart(cs, bp)
	struct ccd_softc *cs;
	struct buf *bp;
{
	long bcount, rcount;
	struct ccdbuf **cbpp, *cbp;
	caddr_t addr;
	daddr_t bn;
	struct partition *pp;
	int i, old_io = ccddevs[cs->sc_unit].ccd_flags & CCDF_OLD;

#ifdef DEBUG
	if (ccddebug & CCDB_FOLLOW)
		printf("ccdstart(%p, %p)\n", cs, bp);
#endif

	/* Instrumentation. */
	disk_busy(&cs->sc_dkdev);

	/*
	 * Translate the partition-relative block number to an absolute.
	 */
	bn = bp->b_blkno;
	if (DISKPART(bp->b_dev) != RAW_PART) {
		pp = &cs->sc_dkdev.dk_label->d_partitions[DISKPART(bp->b_dev)];
		bn += pp->p_offset;
	}

	/*
	 * Allocate component buffers
	 */
	cbpp = malloc(2 * cs->sc_nccdisks * sizeof(struct ccdbuf *), M_DEVBUF,
	    M_WAITOK);
	bzero(cbpp, 2 * cs->sc_nccdisks * sizeof(struct ccdbuf *));
	addr = bp->b_data;
	old_io = old_io || ((vaddr_t)addr & PAGE_MASK);
	for (bcount = bp->b_bcount; bcount > 0; bcount -= rcount) {
		rcount = ccdbuffer(cs, bp, bn, addr, bcount, cbpp, old_io);
		
		/*
		 * This is the old, slower, but less restrictive, mode of
		 * operation.  It allows interleaves which are not multiples
		 * of PAGE_SIZE and mirroring.
		 */
		if (old_io) {
			if ((cbpp[0]->cb_buf.b_flags & B_READ) == 0)
				cbpp[0]->cb_buf.b_vp->v_numoutput++;
			VOP_STRATEGY(&cbpp[0]->cb_buf);

			/*
			 * Mirror requires additional write.
			 */
			if ((cs->sc_cflags & CCDF_MIRROR) &&
			    ((cbpp[0]->cb_buf.b_flags & B_READ) == 0)) {
				cbpp[1]->cb_buf.b_vp->v_numoutput++;
				VOP_STRATEGY(&cbpp[1]->cb_buf);
			}
		}

		bn += btodb(rcount);
		addr += rcount;
	}

	/* The new leaner mode of operation */
	if (!old_io)
		/*
		 * Fire off the requests
		 */
		for (i = 0; i < cs->sc_nccdisks; i++) {
			cbp = cbpp[i];
			if (cbp) {
				if ((cbp->cb_buf.b_flags & B_READ) == 0)
					cbp->cb_buf.b_vp->v_numoutput++;
				VOP_STRATEGY(&cbp->cb_buf);
			}
		}
	free(cbpp, M_DEVBUF);
}

/*
 * Build a component buffer header.
 */
long
ccdbuffer(cs, bp, bn, addr, bcount, cbpp, old_io)
	struct ccd_softc *cs;
	struct buf *bp;
	daddr_t bn;
	caddr_t addr;
	long bcount;
	struct ccdbuf **cbpp;
	int old_io;
{
	struct ccdcinfo *ci, *ci2 = NULL;
	struct ccdbuf *cbp;
	daddr_t cbn, cboff, sblk;
	int ccdisk, off;
	long old_bcount, cnt;
	struct ccdiinfo *ii;
	struct buf *nbp;

#ifdef DEBUG
	if (ccddebug & CCDB_IO)
		printf("ccdbuffer(%p, %p, %d, %p, %ld)\n", cs, bp, bn, addr,
		    bcount);
#endif

	/*
	 * Determine which component bn falls in.
	 */
	cbn = bn;
	cboff = 0;

	if (cs->sc_ileave == 0) {
		/*
		 * Serially concatenated
		 */
		sblk = 0;
		for (ccdisk = 0, ci = &cs->sc_cinfo[ccdisk];
		    cbn >= sblk + ci->ci_size;
		    ccdisk++, ci = &cs->sc_cinfo[ccdisk])
			sblk += ci->ci_size;
		cbn -= sblk;
	} else {
		/*
		 * Interleaved
		 */
		cboff = cbn % cs->sc_ileave;
		cbn /= cs->sc_ileave;
		for (ii = cs->sc_itable; ii->ii_ndisk; ii++)
			if (ii->ii_startblk > cbn)
				break;
		ii--;
		off = cbn - ii->ii_startblk;
		if (ii->ii_ndisk == 1) {
			ccdisk = ii->ii_index[0];
			cbn = ii->ii_startoff + off;
		} else {
			if (cs->sc_cflags & CCDF_MIRROR) {
				ccdisk =
				    ii->ii_index[off % (ii->ii_ndisk / 2)];
				cbn = ii->ii_startoff +
				    (off / (ii->ii_ndisk / 2));
				/* Mirrored data */
				ci2 =
				    &cs->sc_cinfo[ccdisk + (ii->ii_ndisk / 2)];
			} else {
				/* Normal case. */
				ccdisk = ii->ii_index[off % ii->ii_ndisk];
				cbn = ii->ii_startoff + off / ii->ii_ndisk;
			}
		}
		cbn *= cs->sc_ileave;
		ci = &cs->sc_cinfo[ccdisk];
	}

	/* Limit the operation at next component border */
	if (cs->sc_ileave == 0)
		cnt = dbtob(ci->ci_size - cbn);
	else
		cnt = dbtob(cs->sc_ileave - cboff);
	if (cnt < bcount)
		bcount = cnt;

	if (old_io || cbpp[ccdisk] == NULL) {
		/*
		 * Setup new component buffer.
		 */
		cbp = cbpp[old_io ? 0 : ccdisk] = getccdbuf();
		cbp->cb_flags = old_io ? CBF_OLD : 0;
		nbp = &cbp->cb_buf;
		nbp->b_flags = bp->b_flags | B_CALL;
		nbp->b_iodone = ccdiodone;
		nbp->b_proc = bp->b_proc;
		nbp->b_dev = ci->ci_dev;		/* XXX */
		nbp->b_blkno = cbn + cboff;
		nbp->b_vp = ci->ci_vp;
		nbp->b_bcount = bcount;
		LIST_INIT(&nbp->b_dep);

		/*
		 * context for ccdiodone
		 */
		cbp->cb_obp = bp;
		cbp->cb_unit = cs->sc_unit;
		cbp->cb_comp = ccdisk;

		/* Deal with the different algorithms */
		if (old_io)
			nbp->b_data = addr;
		else {
			do {
				nbp->b_data = (caddr_t) uvm_km_valloc(ccdmap,
							    bp->b_bcount);

				/*
				 * XXX Instead of sleeping, we might revert
				 * XXX to old I/O policy for this buffer set.
				 */
				if (nbp->b_data == NULL) {
					ccd_need_kvm++;
					tsleep(ccdmap, PRIBIO, "ccdbuffer", 0);
				}
			} while (nbp->b_data == NULL);
			cbp->cb_sgcnt = 0;
			old_bcount = 0;
		}

		/*
		 * Mirrors have an additional write operation that is nearly
		 * identical to the first.
		 */
		if ((cs->sc_cflags & CCDF_MIRROR) &&
		    ((cbp->cb_buf.b_flags & B_READ) == 0)) {
			cbp = getccdbuf();
			*cbp = *cbpp[0];
			cbp->cb_flags = CBF_MIRROR | (old_io ? CBF_OLD : 0);
			cbp->cb_buf.b_dev = ci2->ci_dev;	/* XXX */
			cbp->cb_buf.b_vp = ci2->ci_vp;
			LIST_INIT(&cbp->cb_buf.b_dep);
			cbp->cb_comp = ci2 - cs->sc_cinfo;
			cbpp[1] = cbp;
		}
	} else {
		/*
		 * Continue on an already started component buffer
		 */
		cbp = cbpp[ccdisk];
		nbp = &cbp->cb_buf;

		/*
		 * Map the new pages at the end of the buffer.
		 */
		old_bcount = nbp->b_bcount;
		nbp->b_bcount += bcount;
	}

	if (!old_io) {
#ifdef DEBUG
		if (ccddebug & CCDB_IO)
			printf("ccdbuffer: sg %d (%p/%x) off %x\n",
			    cbp->cb_sgcnt, addr, bcount, old_bcount);
#endif
		pagemove(addr, nbp->b_data + old_bcount, round_page(bcount));
		nbp->b_bufsize += round_page(bcount);
		cbp->cb_sg[cbp->cb_sgcnt].cs_sgaddr = addr;
		cbp->cb_sg[cbp->cb_sgcnt].cs_sglen = bcount;
		cbp->cb_sgcnt++;
	}

#ifdef DEBUG
	if (ccddebug & CCDB_IO)
		printf(" dev %x(u%d): cbp %p bn %d addr %p bcnt %ld\n",
		    ci->ci_dev, ci-cs->sc_cinfo, cbp, bp->b_blkno,
		    bp->b_data, bp->b_bcount);
#endif

	return (bcount);
}

void
ccdintr(cs, bp)
	struct ccd_softc *cs;
	struct buf *bp;
{

	splassert(IPL_BIO);

#ifdef DEBUG
	if (ccddebug & CCDB_FOLLOW)
		printf("ccdintr(%p, %p)\n", cs, bp);
#endif
	/*
	 * Request is done for better or worse, wakeup the top half.
	 */
	if (bp->b_flags & B_ERROR)
		bp->b_resid = bp->b_bcount;
	disk_unbusy(&cs->sc_dkdev, (bp->b_bcount - bp->b_resid));
	biodone(bp);
}

/*
 * Called at interrupt time.
 * Mark the component as done and if all components are done,
 * take a ccd interrupt.
 */
void
ccdiodone(vbp)
	struct buf *vbp;
{
	struct ccdbuf *cbp = (struct ccdbuf *)vbp;
	struct buf *bp = cbp->cb_obp;
	int unit = cbp->cb_unit;
	struct ccd_softc *cs = &ccd_softc[unit];
	int old_io = cbp->cb_flags & CBF_OLD;
	int cbflags, i;
	long count = bp->b_bcount, off;
	char *comptype;

	splassert(IPL_BIO);

#ifdef DEBUG
	if (ccddebug & CCDB_FOLLOW)
		printf("ccdiodone(%p)\n", cbp);
	if (ccddebug & CCDB_IO) {
		if (cbp->cb_flags & CBF_MIRROR)
			printf("ccdiodone: mirror component\n");
		else
			printf("ccdiodone: bp %p bcount %ld resid %ld\n",
			    bp, bp->b_bcount, bp->b_resid);
		printf(" dev %x(u%d), cbp %p bn %d addr %p bcnt %ld\n",
		    vbp->b_dev, cbp->cb_comp, cbp, vbp->b_blkno,
		    vbp->b_data, vbp->b_bcount);
	}
#endif

	if (vbp->b_flags & B_ERROR) {
		if (cbp->cb_flags & CBF_MIRROR)
			comptype = " (mirror)";
		else {
			bp->b_flags |= B_ERROR;
			bp->b_error = vbp->b_error ?
			    vbp->b_error : EIO;
			comptype = "";
		}

		printf("%s: error %d on component %d%s\n",
		    cs->sc_xname, bp->b_error, cbp->cb_comp, comptype);
	}
	cbflags = cbp->cb_flags;

	if (!old_io) {
		/*
		 * Gather all the pieces and put them where they should be.
		 */
		for (i = 0, off = 0; i < cbp->cb_sgcnt; i++) {
#ifdef DEBUG
			if (ccddebug & CCDB_IO)
				printf("ccdiodone: sg %d (%p/%x) off %x\n", i,
				    cbp->cb_sg[i].cs_sgaddr,
				    cbp->cb_sg[i].cs_sglen, off);
#endif
			pagemove(vbp->b_data + off, cbp->cb_sg[i].cs_sgaddr,
			    round_page(cbp->cb_sg[i].cs_sglen));
			off += cbp->cb_sg[i].cs_sglen;
		}

		uvm_km_free(ccdmap, (vaddr_t)vbp->b_data, count);
		if (ccd_need_kvm) {
			ccd_need_kvm = 0;
			wakeup(ccdmap);
		}
	}
	count = vbp->b_bcount;
	putccdbuf(cbp);

	if ((cbflags & CBF_MIRROR) == 0) {
		/*
		 * If all done, "interrupt".
		 *
		 * Note that mirror component buffers aren't counted against
		 * the original I/O buffer.
		 */
		if (count > bp->b_resid)
			panic("ccdiodone: count");
		bp->b_resid -= count;
		if (bp->b_resid == 0)
			ccdintr(&ccd_softc[unit], bp);
	}
}

/* ARGSUSED */
int
ccdread(dev, uio, flags)
	dev_t dev;
	struct uio *uio;
	int flags;
{
	int unit = ccdunit(dev);
	struct ccd_softc *cs;

#ifdef DEBUG
	if (ccddebug & CCDB_FOLLOW)
		printf("ccdread(%x, %p)\n", dev, uio);
#endif
	if (unit >= numccd)
		return (ENXIO);
	cs = &ccd_softc[unit];

	if ((cs->sc_flags & CCDF_INITED) == 0)
		return (ENXIO);

	/*
	 * XXX: It's not clear that using minphys() is completely safe,
	 * in particular, for raw I/O.  Underlying devices might have some
	 * non-obvious limits, because of the copy to user-space.
	 */
	return (physio(ccdstrategy, NULL, dev, B_READ, minphys, uio));
}

/* ARGSUSED */
int
ccdwrite(dev, uio, flags)
	dev_t dev;
	struct uio *uio;
	int flags;
{
	int unit = ccdunit(dev);
	struct ccd_softc *cs;

#ifdef DEBUG
	if (ccddebug & CCDB_FOLLOW)
		printf("ccdwrite(%x, %p)\n", dev, uio);
#endif
	if (unit >= numccd)
		return (ENXIO);
	cs = &ccd_softc[unit];

	if ((cs->sc_flags & CCDF_INITED) == 0)
		return (ENXIO);

	/*
	 * XXX: It's not clear that using minphys() is completely safe,
	 * in particular, for raw I/O.  Underlying devices might have some
	 * non-obvious limits, because of the copy to user-space.
	 */
	return (physio(ccdstrategy, NULL, dev, B_WRITE, minphys, uio));
}

int
ccdioctl(dev, cmd, data, flag, p)
	dev_t dev;
	u_long cmd;
	caddr_t data;
	int flag;
	struct proc *p;
{
	int unit = ccdunit(dev);
	int i, j, lookedup = 0, error = 0;
	int part, pmask, s;
	struct ccd_softc *cs;
	struct ccd_ioctl *ccio = (struct ccd_ioctl *)data;
	struct ccddevice ccd;
	char **cpp;
	struct vnode **vpp;
	vaddr_t min, max;

	if (unit >= numccd)
		return (ENXIO);
	cs = &ccd_softc[unit];

	bzero(&ccd, sizeof(ccd));

	switch (cmd) {
	case CCDIOCSET:
		if (cs->sc_flags & CCDF_INITED)
			return (EBUSY);

		if ((flag & FWRITE) == 0)
			return (EBADF);

		if ((error = ccdlock(cs)) != 0)
			return (error);

		/* Fill in some important bits. */
		ccd.ccd_unit = unit;
		ccd.ccd_interleave = ccio->ccio_ileave;
		ccd.ccd_flags = ccio->ccio_flags & CCDF_USERMASK;

		/* XXX the new code is unstable still */
		ccd.ccd_flags |= CCDF_OLD;

		/*
		 * Interleaving which is not a multiple of the click size
		 * must use the old I/O code (by design), as must mirror
		 * setups (until implemented in the new code).
		 */
		if (ccio->ccio_ileave % (PAGE_SIZE / DEV_BSIZE) != 0 ||
		    (ccd.ccd_flags & CCDF_MIRROR))
			ccd.ccd_flags |= CCDF_OLD;

		/*
		 * Allocate space for and copy in the array of
		 * componet pathnames and device numbers.
		 */
		cpp = malloc(ccio->ccio_ndisks * sizeof(char *),
		    M_DEVBUF, M_WAITOK);
		vpp = malloc(ccio->ccio_ndisks * sizeof(struct vnode *),
		    M_DEVBUF, M_WAITOK);

		error = copyin((caddr_t)ccio->ccio_disks, (caddr_t)cpp,
		    ccio->ccio_ndisks * sizeof(char **));
		if (error) {
			free(vpp, M_DEVBUF);
			free(cpp, M_DEVBUF);
			ccdunlock(cs);
			return (error);
		}

#ifdef DEBUG
		if (ccddebug & CCDB_INIT)
			for (i = 0; i < ccio->ccio_ndisks; ++i)
				printf("ccdioctl: component %d: 0x%p\n",
				    i, cpp[i]);
#endif

		for (i = 0; i < ccio->ccio_ndisks; ++i) {
#ifdef DEBUG
			if (ccddebug & CCDB_INIT)
				printf("ccdioctl: lookedup = %d\n", lookedup);
#endif
			if ((error = ccdlookup(cpp[i], p, &vpp[i])) != 0) {
				for (j = 0; j < lookedup; ++j)
					(void)vn_close(vpp[j], FREAD|FWRITE,
					    p->p_ucred, p);
				free(vpp, M_DEVBUF);
				free(cpp, M_DEVBUF);
				ccdunlock(cs);
				return (error);
			}
			++lookedup;
		}
		ccd.ccd_cpp = cpp;
		ccd.ccd_vpp = vpp;
		ccd.ccd_ndev = ccio->ccio_ndisks;

		/*
		 * Initialize the ccd.  Fills in the softc for us.
		 */
		if ((error = ccdinit(&ccd, cpp, p)) != 0) {
			for (j = 0; j < lookedup; ++j)
				(void)vn_close(vpp[j], FREAD|FWRITE,
				    p->p_ucred, p);
			bzero(&ccd_softc[unit], sizeof(struct ccd_softc));
			free(vpp, M_DEVBUF);
			free(cpp, M_DEVBUF);
			ccdunlock(cs);
			return (error);
		}

		/*
		 * The ccd has been successfully initialized, so
		 * we can place it into the array.  Don't try to
		 * read the disklabel until the disk has been attached,
		 * because space for the disklabel is allocated
		 * in disk_attach();
		 */
		bcopy(&ccd, &ccddevs[unit], sizeof(ccd));
		ccio->ccio_unit = unit;
		ccio->ccio_size = cs->sc_size;

		/*
		 * If we use the optimized protocol we need some kvm space
		 * for the component buffers.  Allocate it here.
		 *
		 * XXX I'd like to have a more dynamic way of acquiring kvm
		 * XXX space, but that is problematic as we are not allowed
		 * XXX to lock the kernel_map in interrupt context.  It is
		 * XXX doable via a freelist implementation though.
		 */
		if (!ccdmap && !(ccd.ccd_flags & CCDF_OLD))
			ccdmap = uvm_km_suballoc(kernel_map, &min, &max,
			    CCD_CLUSTERS * MAXBSIZE, VM_MAP_INTRSAFE,
			    FALSE, NULL);

		/* Attach the disk. */
		cs->sc_dkdev.dk_name = cs->sc_xname;
		disk_attach(&cs->sc_dkdev);

		/* Try and read the disklabel. */
		ccdgetdisklabel(dev);

		ccdunlock(cs);

		break;

	case CCDIOCCLR:
		if ((cs->sc_flags & CCDF_INITED) == 0)
			return (ENXIO);

		if ((flag & FWRITE) == 0)
			return (EBADF);

		if ((error = ccdlock(cs)) != 0)
			return (error);

		/*
		 * Don't unconfigure if any other partitions are open
		 * or if both the character and block flavors of this
		 * partition are open.
		 */
		part = DISKPART(dev);
		pmask = (1 << part);
		if ((cs->sc_dkdev.dk_openmask & ~pmask) ||
		    ((cs->sc_dkdev.dk_bopenmask & pmask) &&
		    (cs->sc_dkdev.dk_copenmask & pmask))) {
			ccdunlock(cs);
			return (EBUSY);
		}

		/*
		 * Free ccd_softc information and clear entry.
		 */

		/* Close the components and free their pathnames. */
		for (i = 0; i < cs->sc_nccdisks; ++i) {
			/*
			 * XXX: this close could potentially fail and
			 * cause Bad Things.  Maybe we need to force
			 * the close to happen?
			 */
#ifdef DEBUG
			if (ccddebug & CCDB_VNODE)
				vprint("CCDIOCCLR: vnode info",
				    cs->sc_cinfo[i].ci_vp);
#endif
			(void)vn_close(cs->sc_cinfo[i].ci_vp, FREAD|FWRITE,
			    p->p_ucred, p);
			free(cs->sc_cinfo[i].ci_path, M_DEVBUF);
		}

		/* Free interleave index. */
		for (i = 0; cs->sc_itable[i].ii_ndisk; ++i)
			free(cs->sc_itable[i].ii_index, M_DEVBUF);

		/* Free component info and interleave table. */
		free(cs->sc_cinfo, M_DEVBUF);
		free(cs->sc_itable, M_DEVBUF);
		cs->sc_flags &= ~CCDF_INITED;

		/*
		 * Free ccddevice information and clear entry.
		 */
		free(ccddevs[unit].ccd_cpp, M_DEVBUF);
		free(ccddevs[unit].ccd_vpp, M_DEVBUF);
		bcopy(&ccd, &ccddevs[unit], sizeof(ccd));

		/* Detatch the disk. */
		disk_detach(&cs->sc_dkdev);

		/* This must be atomic. */
		s = splhigh();
		ccdunlock(cs);
		bzero(cs, sizeof(struct ccd_softc));
		splx(s);

		break;

	case DIOCGDINFO:
		if ((cs->sc_flags & CCDF_INITED) == 0)
			return (ENXIO);

		*(struct disklabel *)data = *(cs->sc_dkdev.dk_label);
		break;

	case DIOCGPART:
		if ((cs->sc_flags & CCDF_INITED) == 0)
			return (ENXIO);

		((struct partinfo *)data)->disklab = cs->sc_dkdev.dk_label;
		((struct partinfo *)data)->part =
		    &cs->sc_dkdev.dk_label->d_partitions[DISKPART(dev)];
		break;

	case DIOCWDINFO:
	case DIOCSDINFO:
		if ((cs->sc_flags & CCDF_INITED) == 0)
			return (ENXIO);

		if ((flag & FWRITE) == 0)
			return (EBADF);

		if ((error = ccdlock(cs)) != 0)
			return (error);

		cs->sc_flags |= CCDF_LABELLING;

		error = setdisklabel(cs->sc_dkdev.dk_label,
		    (struct disklabel *)data, 0, cs->sc_dkdev.dk_cpulabel);
		if (error == 0) {
			if (cmd == DIOCWDINFO)
				error = writedisklabel(CCDLABELDEV(dev),
				    ccdstrategy, cs->sc_dkdev.dk_label,
				    cs->sc_dkdev.dk_cpulabel);
		}

		cs->sc_flags &= ~CCDF_LABELLING;

		ccdunlock(cs);

		if (error)
			return (error);
		break;

	case DIOCWLABEL:
		if ((cs->sc_flags & CCDF_INITED) == 0)
			return (ENXIO);

		if ((flag & FWRITE) == 0)
			return (EBADF);
		if (*(int *)data != 0)
			cs->sc_flags |= CCDF_WLABEL;
		else
			cs->sc_flags &= ~CCDF_WLABEL;
		break;

	default:
		return (ENOTTY);
	}

	return (0);
}

int
ccdsize(dev)
	dev_t dev;
{
	struct ccd_softc *cs;
	int part, size, unit;

	unit = ccdunit(dev);
	if (unit >= numccd)
		return (-1);

	cs = &ccd_softc[unit];
	if ((cs->sc_flags & CCDF_INITED) == 0)
		return (-1);

	if (ccdopen(dev, 0, S_IFBLK, curproc))
		return (-1);

	part = DISKPART(dev);
	if (cs->sc_dkdev.dk_label->d_partitions[part].p_fstype != FS_SWAP)
		size = -1;
	else
		size = cs->sc_dkdev.dk_label->d_partitions[part].p_size;

	if (ccdclose(dev, 0, S_IFBLK, curproc))
		return (-1);

	return (size);
}

int
ccddump(dev, blkno, va, size)
	dev_t dev;
	daddr_t blkno;
	caddr_t va;
	size_t size;
{

	/* Not implemented. */
	return ENXIO;
}

/*
 * Lookup the provided name in the filesystem.  If the file exists,
 * is a valid block device, and isn't being used by anyone else,
 * set *vpp to the file's vnode.
 */
int
ccdlookup(path, p, vpp)
	char *path;
	struct proc *p;
	struct vnode **vpp;	/* result */
{
	struct nameidata nd;
	struct vnode *vp;
	struct vattr va;
	int error;

	NDINIT(&nd, LOOKUP, FOLLOW, UIO_USERSPACE, path, p);
	if ((error = vn_open(&nd, FREAD|FWRITE, 0)) != 0) {
#ifdef DEBUG
		if (ccddebug & (CCDB_FOLLOW|CCDB_INIT))
			printf("ccdlookup: vn_open error = %d\n", error);
#endif
		return (error);
	}
	vp = nd.ni_vp;

	if (vp->v_usecount > 1) {
		VOP_UNLOCK(vp, 0, p);
		(void)vn_close(vp, FREAD|FWRITE, p->p_ucred, p);
		return (EBUSY);
	}

	if ((error = VOP_GETATTR(vp, &va, p->p_ucred, p)) != 0) {
#ifdef DEBUG
		if (ccddebug & (CCDB_FOLLOW|CCDB_INIT))
			printf("ccdlookup: getattr error = %d\n", error);
#endif
		VOP_UNLOCK(vp, 0, p);
		(void)vn_close(vp, FREAD|FWRITE, p->p_ucred, p);
		return (error);
	}

	/* XXX: eventually we should handle VREG, too. */
	if (va.va_type != VBLK) {
		VOP_UNLOCK(vp, 0, p);
		(void)vn_close(vp, FREAD|FWRITE, p->p_ucred, p);
		return (ENOTBLK);
	}

#ifdef DEBUG
	if (ccddebug & CCDB_VNODE)
		vprint("ccdlookup: vnode info", vp);
#endif

	VOP_UNLOCK(vp, 0, p);
	*vpp = vp;
	return (0);
}

/*
 * Read the disklabel from the ccd.  If one is not present, fake one
 * up.
 */
void
ccdgetdisklabel(dev)
	dev_t dev;
{
	int unit = ccdunit(dev);
	struct ccd_softc *cs = &ccd_softc[unit];
	char *errstring;
	struct disklabel *lp = cs->sc_dkdev.dk_label;
	struct cpu_disklabel *clp = cs->sc_dkdev.dk_cpulabel;
	struct ccdgeom *ccg = &cs->sc_geom;

	bzero(lp, sizeof(*lp));
	bzero(clp, sizeof(*clp));

	lp->d_secperunit = cs->sc_size;
	lp->d_secsize = ccg->ccg_secsize;
	lp->d_nsectors = ccg->ccg_nsectors;
	lp->d_ntracks = ccg->ccg_ntracks;
	lp->d_ncylinders = ccg->ccg_ncylinders;
	lp->d_secpercyl = lp->d_ntracks * lp->d_nsectors;

	strncpy(lp->d_typename, "ccd", sizeof(lp->d_typename));
	lp->d_type = DTYPE_CCD;
	strncpy(lp->d_packname, "fictitious", sizeof(lp->d_packname));
	lp->d_rpm = 3600;
	lp->d_interleave = 1;
	lp->d_flags = 0;

	lp->d_partitions[RAW_PART].p_offset = 0;
	lp->d_partitions[RAW_PART].p_size = cs->sc_size;
	lp->d_partitions[RAW_PART].p_fstype = FS_UNUSED;
	lp->d_npartitions = RAW_PART + 1;

	lp->d_magic = DISKMAGIC;
	lp->d_magic2 = DISKMAGIC;
	lp->d_checksum = dkcksum(cs->sc_dkdev.dk_label);

	/*
	 * Call the generic disklabel extraction routine.
	 */
	errstring = readdisklabel(CCDLABELDEV(dev), ccdstrategy,
	    cs->sc_dkdev.dk_label, cs->sc_dkdev.dk_cpulabel, 0);
	if (errstring)
		ccdmakedisklabel(cs);

#ifdef DEBUG
	/* It's actually extremely common to have unlabeled ccds. */
	if (ccddebug & CCDB_LABEL)
		if (errstring != NULL)
			printf("%s: %s\n", cs->sc_xname, errstring);
#endif
}

/*
 * Take care of things one might want to take care of in the event
 * that a disklabel isn't present.
 */
void
ccdmakedisklabel(cs)
	struct ccd_softc *cs;
{
	struct disklabel *lp = cs->sc_dkdev.dk_label;

	/*
	 * For historical reasons, if there's no disklabel present
	 * the raw partition must be marked FS_BSDFFS.
	 */
	lp->d_partitions[RAW_PART].p_fstype = FS_BSDFFS;

	strncpy(lp->d_packname, "default label", sizeof(lp->d_packname));
}

/*
 * Wait interruptibly for an exclusive lock.
 *
 * XXX
 * Several drivers do this; it should be abstracted and made MP-safe.
 */
int
ccdlock(cs)
	struct ccd_softc *cs;
{
	int error;

	while ((cs->sc_flags & CCDF_LOCKED) != 0) {
		cs->sc_flags |= CCDF_WANTED;
		if ((error = tsleep(cs, PRIBIO | PCATCH, "ccdlck", 0)) != 0)
			return (error);
	}
	cs->sc_flags |= CCDF_LOCKED;
	return (0);
}

/*
 * Unlock and wake up any waiters.
 */
void
ccdunlock(cs)
	struct ccd_softc *cs;
{

	cs->sc_flags &= ~CCDF_LOCKED;
	if ((cs->sc_flags & CCDF_WANTED) != 0) {
		cs->sc_flags &= ~CCDF_WANTED;
		wakeup(cs);
	}
}

#ifdef DEBUG
void
printiinfo(ii)
	struct ccdiinfo *ii;
{
	int ix, i;

	for (ix = 0; ii->ii_ndisk; ix++, ii++) {
		printf(" itab[%d]: #dk %d sblk %d soff %d",
		       ix, ii->ii_ndisk, ii->ii_startblk, ii->ii_startoff);
		for (i = 0; i < ii->ii_ndisk; i++)
			printf(" %d", ii->ii_index[i]);
		printf("\n");
	}
}
#endif
