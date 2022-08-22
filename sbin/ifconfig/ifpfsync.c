/*-
 * SPDX-License-Identifier: BSD-2-Clause-FreeBSD
 *
 * Copyright (c) 2003 Ryan McBride. All rights reserved.
 * Copyright (c) 2004 Max Laier. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $FreeBSD$
 */

#include <sys/param.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

#include <net/if.h>
#include <netinet/in.h>
#include <net/pfvar.h>
#include <net/if_pfsync.h>
#include <net/route.h>
#include <arpa/inet.h>

#include <err.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ifconfig.h"

void setpfsync_syncdev(const char *, int, int, const struct afswtch *);
void unsetpfsync_syncdev(const char *, int, int, const struct afswtch *);
void setpfsync_syncpeer(const char *, int, int, const struct afswtch *);
void unsetpfsync_syncpeer(const char *, int, int, const struct afswtch *);
void setpfsync_syncpeer(const char *, int, int, const struct afswtch *);
void setpfsync_maxupd(const char *, int, int, const struct afswtch *);
void setpfsync_defer(const char *, int, int, const struct afswtch *);
void pfsync_status(int);

void
setpfsync_syncdev(const char *val, int d, int s, const struct afswtch *rafp)
{
	struct pfsyncreq preq;

	bzero((char *)&preq, sizeof(struct pfsyncreq));
	ifr.ifr_data = (caddr_t)&preq;

	if (ioctl(s, SIOCGETPFSYNC, (caddr_t)&ifr) == -1)
		err(1, "SIOCGETPFSYNC");

	strlcpy(preq.pfsyncr_syncdev, val, sizeof(preq.pfsyncr_syncdev));

	if (ioctl(s, SIOCSETPFSYNC, (caddr_t)&ifr) == -1)
		err(1, "SIOCSETPFSYNC");
}

/* ARGSUSED */
void
unsetpfsync_syncdev(const char *val, int d, int s, const struct afswtch *rafp)
{
	struct pfsyncreq preq;

	bzero((char *)&preq, sizeof(struct pfsyncreq));
	ifr.ifr_data = (caddr_t)&preq;

	if (ioctl(s, SIOCGETPFSYNC, (caddr_t)&ifr) == -1)
		err(1, "SIOCGETPFSYNC");

	bzero((char *)&preq.pfsyncr_syncdev, sizeof(preq.pfsyncr_syncdev));

	if (ioctl(s, SIOCSETPFSYNC, (caddr_t)&ifr) == -1)
		err(1, "SIOCSETPFSYNC");
}

/* ARGSUSED */
void
setpfsync_syncpeer(const char *val, int d, int s, const struct afswtch *rafp)
{
	struct pfsyncreq preq;
	struct addrinfo *peerres;
	int ecode;

	bzero((char *)&preq, sizeof(struct pfsyncreq));
	ifr.ifr_data = (caddr_t)&preq;

	if (ioctl(s, SIOCGETPFSYNC, (caddr_t)&ifr) == -1)
		err(1, "SIOCGETPFSYNC");

	if ((ecode = getaddrinfo(val, NULL, NULL, &peerres)) != 0)
		errx(1, "error in parsing address string: %s",
		    gai_strerror(ecode));

	switch (peerres->ai_family) {
#ifdef INET
	case AF_INET: {
		struct sockaddr_in *sin = (struct sockaddr_in *)peerres->ai_addr;

		if (IN_MULTICAST(ntohl(sin->sin_addr.s_addr)))
			errx(1, "syncpeer address cannot be multicast");

		preq.pfsyncr_syncpeer.in4 = *sin;
		break;
	}
#endif
	default:
		errx(1, "syncpeer address %s not supported", val);
	}

	if (ioctl(s, SIOCSETPFSYNC, (caddr_t)&ifr) == -1)
		err(1, "SIOCSETPFSYNC");
	freeaddrinfo(peerres);
}

/* ARGSUSED */
void
unsetpfsync_syncpeer(const char *val, int d, int s, const struct afswtch *rafp)
{
	struct pfsyncreq preq;

	bzero((char *)&preq, sizeof(struct pfsyncreq));
	ifr.ifr_data = (caddr_t)&preq;

	if (ioctl(s, SIOCGETPFSYNC, (caddr_t)&ifr) == -1)
		err(1, "SIOCGETPFSYNC");

	switch (preq.pfsyncr_syncpeer.sa.sa_family) {
#ifdef INET
	case AF_INET:
	{
		memset((char *)&preq.pfsyncr_syncpeer, 0,
		    sizeof(union pfsync_sockaddr));
		break;
	}
#endif
	}

	if (ioctl(s, SIOCSETPFSYNC, (caddr_t)&ifr) == -1)
		err(1, "SIOCSETPFSYNC");
}

/* ARGSUSED */
void
setpfsync_maxupd(const char *val, int d, int s, const struct afswtch *rafp)
{
	struct pfsyncreq preq;
	int maxupdates;

	maxupdates = atoi(val);
	if ((maxupdates < 0) || (maxupdates > 255))
		errx(1, "maxupd %s: out of range", val);

	memset((char *)&preq, 0, sizeof(struct pfsyncreq));
	ifr.ifr_data = (caddr_t)&preq;

	if (ioctl(s, SIOCGETPFSYNC, (caddr_t)&ifr) == -1)
		err(1, "SIOCGETPFSYNC");

	preq.pfsyncr_maxupdates = maxupdates;

	if (ioctl(s, SIOCSETPFSYNC, (caddr_t)&ifr) == -1)
		err(1, "SIOCSETPFSYNC");
}

/* ARGSUSED */
void
setpfsync_defer(const char *val, int d, int s, const struct afswtch *rafp)
{
	struct pfsyncreq preq;

	memset((char *)&preq, 0, sizeof(struct pfsyncreq));
	ifr.ifr_data = (caddr_t)&preq;

	if (ioctl(s, SIOCGETPFSYNC, (caddr_t)&ifr) == -1)
		err(1, "SIOCGETPFSYNC");

	preq.pfsyncr_defer = d ? PFSYNCF_DEFER : 0;
	if (ioctl(s, SIOCSETPFSYNC, (caddr_t)&ifr) == -1)
		err(1, "SIOCSETPFSYNC");
}

void
pfsync_status(int s)
{
	struct pfsyncreq preq;
	char syncpeer[NI_MAXHOST];
	struct sockaddr *syncpeer_sa;
	int error;

	bzero((char *)&preq, sizeof(struct pfsyncreq));
	ifr.ifr_data = (caddr_t)&preq;
	syncpeer_sa = &preq.pfsyncr_syncpeer.sa;

	if (ioctl(s, SIOCGETPFSYNC, (caddr_t)&ifr) == -1)
		return;

	if (preq.pfsyncr_syncdev[0] != '\0' ||
	    syncpeer_sa->sa_family != AF_UNSPEC)
		printf("\t");

	if (preq.pfsyncr_syncdev[0] != '\0')
		printf("syncdev: %s ", preq.pfsyncr_syncdev);

	if (preq.pfsyncr_syncpeer.sa.sa_family != AF_UNSPEC &&
	    preq.pfsyncr_syncpeer.in4.sin_addr.s_addr != htonl(INADDR_PFSYNC_GROUP)) {
		if ((error = getnameinfo(syncpeer_sa, syncpeer_sa->sa_len,
		    syncpeer, sizeof(syncpeer), NULL, 0, NI_NUMERICHOST)) != 0)
			errx(1, "getnameinfo: %s", gai_strerror(error));
		printf("syncpeer: %s ", syncpeer);
	}

	if (preq.pfsyncr_syncdev[0] != '\0' ||
	    (preq.pfsyncr_syncpeer.sa.sa_family != AF_UNSPEC &&
		preq.pfsyncr_syncpeer.in4.sin_addr.s_addr != htonl(INADDR_PFSYNC_GROUP))) {
		printf("maxupd: %d ", preq.pfsyncr_maxupdates);
		printf("defer: %s\n",
		    (preq.pfsyncr_defer & PFSYNCF_DEFER) ? "on" : "off");
		printf("\tsyncok: %d\n",
		    (preq.pfsyncr_defer & PFSYNCF_OK) ? 1 : 0);
	}
}

static struct cmd pfsync_cmds[] = {
	DEF_CMD_ARG("syncdev",		setpfsync_syncdev),
	DEF_CMD("-syncdev",	1,	unsetpfsync_syncdev),
	DEF_CMD_ARG("syncif",		setpfsync_syncdev),
	DEF_CMD("-syncif",	1,	unsetpfsync_syncdev),
	DEF_CMD_ARG("syncpeer",		setpfsync_syncpeer),
	DEF_CMD("-syncpeer",	1,	unsetpfsync_syncpeer),
	DEF_CMD_ARG("maxupd",		setpfsync_maxupd),
	DEF_CMD("defer",	1,	setpfsync_defer),
	DEF_CMD("-defer",	0,	setpfsync_defer),
};
static struct afswtch af_pfsync = {
	.af_name	= "af_pfsync",
	.af_af		= AF_UNSPEC,
	.af_other_status = pfsync_status,
};

static __constructor void
pfsync_ctor(void)
{
	int i;

	for (i = 0; i < nitems(pfsync_cmds);  i++)
		cmd_register(&pfsync_cmds[i]);
	af_register(&af_pfsync);
}
