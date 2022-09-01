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
#include <sys/errno.h>
#include <sys/ioctl.h>
#include <sys/nv.h>
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
void oldstatus(const char *, int, int, const struct afswtch *);

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

		preq.pfsyncr_syncpeer = sin->sin_addr;
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

	preq.pfsyncr_syncpeer.s_addr = 0;
//	switch (preq.pfsyncr_syncpeer.sa.sa_family) {
//#ifdef INET
//	case AF_INET:
//	{
//		memset((char *)&preq.pfsyncr_syncpeer, 0,
//		    sizeof(struct sockaddr_storage));
//		break;
//	}
//#endif
//	}

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

static int
pfsync_syncpeer_nvlist_to_sockaddr(const nvlist_t *nvl,
    struct sockaddr_storage *sa)
{
	int af;

	if (!nvlist_exists_number(nvl, "af"))
		return (EINVAL);
	if (!nvlist_exists_binary(nvl, "address"))
		return (EINVAL);

	af = nvlist_get_number(nvl, "af");

	switch (af) {
#ifdef INET
	case AF_INET: {
		struct sockaddr_in *in = (struct sockaddr_in *)sa;
		size_t len;
		const void *addr = nvlist_get_binary(nvl, "address", &len);
		in->sin_family = af;
		printf("Sanity check: %zu %zu\n", len, sizeof(*in));
		if (len != sizeof(*in))
			return (EINVAL);

		const struct sockaddr_in *test = (const struct sockaddr_in *)addr;
		printf("Sanity check: %d %d\n", test->sin_len, test->sin_family);
		memcpy(in, addr, sizeof(*in));
		break;
	}
#endif
#ifdef INET6
	case AF_INET6: {
		struct sockaddr_in6 *in6 = (struct sockaddr_in6 *)sa;
		size_t len;
		const void *addr = nvlist_get_binary(nvl, "address", &len);
		if (len != sizeof(*in6))
			return (EINVAL);

		memcpy(in6, addr, sizeof(*in6));
		break;
	}
#endif
	default:
		return (EINVAL);
	}

	return (0);
}

static void
pfsync_nvstatus_to_kstatus(const nvlist_t *nvl, struct pfsync_kstatus *status)
{
	struct sockaddr_storage addr;
	int ret;
	printf("Top of pfsync_nvstatus_to_kstatus\n");
	if (nvlist_exists_string(nvl, "syncdev"))
		strlcpy(status->syncdev, nvlist_get_string(nvl, "syncdev"),
		    IFNAMSIZ);
	if (nvlist_exists_number(nvl, "maxupdates"))
		status->maxupdates = nvlist_get_number(nvl, "maxupdates");
	if (nvlist_exists_number(nvl, "flags"))
		status->flags = nvlist_get_number(nvl, "flags");
	if (nvlist_exists_nvlist(nvl, "syncpeer")) {
		ret = pfsync_syncpeer_nvlist_to_sockaddr(nvlist_get_nvlist(nvl, "syncpeer"), &addr);
		if (ret == 0)
			status->syncpeer = addr;
		else
			printf("pfsync_syncpeer_nvlist_to_sockaddr returned %d\n", ret);
	}
}

void
pfsync_status(int s)
{
	struct pfsync_kstatus status;
	char syncpeer[NI_MAXHOST];
	nvlist_t *nvl;
	int error;
	void *data;

	memset((char *)&status, 0, sizeof(struct pfsync_kstatus));
	memset((char *)&syncpeer, 0, NI_MAXHOST);


	data = malloc(IFR_CAP_NV_MAXBUFSIZE);
	if (data == NULL)
		err(1, "malloc");

	ifr.ifr_cap_nv.buffer = data;
	ifr.ifr_cap_nv.buf_length = IFR_CAP_NV_MAXBUFSIZE;
	ifr.ifr_cap_nv.length = 0;

	printf("Before ioctl\n");
	if (ioctl(s, SIOCGETPFSYNCNV, (caddr_t)&ifr) == -1)
		return;
	printf("After ioctl\n");

	nvl = nvlist_unpack(ifr.ifr_cap_nv.buffer,
	    ifr.ifr_cap_nv.length, 0);
	if (nvl == NULL)
		err(1, "nvlist_unpack");

	pfsync_nvstatus_to_kstatus(nvl, &status);

	nvlist_destroy(nvl);
	free(data);

	if (status.syncdev[0] != '\0' ||
	    status.syncpeer.ss_family != AF_UNSPEC)
		printf("\t");

	if (status.syncdev[0] != '\0')
		printf("syncdev: %s ", status.syncdev);

	// TODO: Implement the AF_INET6 part
	if (status.syncpeer.ss_family == AF_INET &&
	    ((struct sockaddr_in *)&(status.syncpeer))->sin_addr.s_addr != htonl(INADDR_PFSYNC_GROUP)) {

		struct sockaddr *syncpeer_sa = (struct sockaddr *)&status.syncpeer;
		if ((error = getnameinfo(syncpeer_sa, syncpeer_sa->sa_len,
			 syncpeer, sizeof(syncpeer), NULL, 0, NI_NUMERICHOST)) != 0)
			errx(1, "getnameinfo: %s", gai_strerror(error));
		printf("syncpeer: %s ", syncpeer);
	}

	printf("maxupd: %d ", status.maxupdates);
	printf("defer: %s\n",
	    (status.flags & PFSYNCF_DEFER) ? "on" : "off");
	printf("\tsyncok: %d\n",
	    (status.flags & PFSYNCF_OK) ? 1 : 0);
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
