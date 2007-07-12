/*-
 * Copyright (c) 2003
 *	Bill Paul <wpaul@windriver.com>.  All rights reserved.
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
 *	This product includes software developed by Bill Paul.
 * 4. Neither the name of the author nor the names of any co-contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY Bill Paul AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL Bill Paul OR THE VOICES IN HIS HEAD
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 *
 * WPA support originally contributed by Arvind Srinivasan <arvind@celar.us>
 * then hacked upon mercilessly by my.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/sockio.h>
#include <sys/mbuf.h>
#include <sys/malloc.h>
#include <sys/endian.h>
#include <sys/priv.h>
#include <sys/kernel.h>
#include <sys/socket.h>
#include <sys/queue.h>
#include <sys/module.h>
#include <sys/proc.h>
#if __FreeBSD_version < 502113
#include <sys/sysctl.h>
#endif
#include <sys/kthread.h>
#include <sys/taskqueue.h>


#include <net/if.h>
#include <net/if_arp.h>
#include <net/ethernet.h>
#include <net/if_dl.h>
#include <net/if_media.h>
#include <net/if_types.h>
#include <net/route.h>

#include <net/bpf.h>

#include <machine/bus.h>
#include <machine/resource.h>
#include <sys/bus.h>
#include <sys/rman.h>

#include <net80211/ieee80211_var.h>
#include <net80211/ieee80211_ioctl.h>
#include <net80211/ieee80211_regdomain.h>

#if __FreeBSD_version < 700046
#include <dev/wi/if_wavelan_ieee.h>
#endif

#include <dev/pci/pcireg.h>
#include <dev/pci/pcivar.h>

#include <compat/ndis/pe_var.h>
#include <compat/ndis/cfg_var.h>
#include <compat/ndis/resource_var.h>
#include <compat/ndis/ntoskrnl_var.h>
#include <compat/ndis/hal_var.h>
#include <compat/ndis/ndis_var.h>
#include <dev/if_ndis/if_ndisvar.h>

#ifdef NDIS_DEBUG
#define DPRINTF(x)	printf x
#else
#define DPRINTF(x)
#endif

MODULE_DEPEND(ndis, ether, 1, 1, 1);
MODULE_DEPEND(ndis, wlan, 1, 1, 1);
MODULE_DEPEND(ndis, ndisapi, 1, 1, 1);

MODULE_VERSION(ndis, 1);

int ndis_attach			(device_t);
int ndis_detach			(device_t);
int ndis_suspend		(device_t);
int ndis_resume			(device_t);
void ndis_shutdown		(device_t);

int ndisdrv_modevent		(module_t, int, void *);

static void ndis_txeof		(ndis_handle, ndis_packet *, ndis_status);
static void ndis_rxeof		(ndis_handle, ndis_packet **, uint32_t);
static void ndis_rxeof_eth	(ndis_handle, ndis_handle, char *, void *,
				 uint32_t, void *, uint32_t, uint32_t);
static void ndis_rxeof_done	(ndis_handle);
static void ndis_rxeof_xfr	(kdpc *, ndis_handle, void *, void *);
static void ndis_rxeof_xfr_done	(ndis_handle, ndis_packet *,
				 uint32_t, uint32_t);
static void ndis_linksts	(ndis_handle, ndis_status, void *, uint32_t);
static void ndis_linksts_done	(ndis_handle);

/* We need to wrap these functions for amd64. */

static funcptr ndis_txeof_wrap;
static funcptr ndis_rxeof_wrap;
static funcptr ndis_rxeof_eth_wrap;
static funcptr ndis_rxeof_done_wrap;
static funcptr ndis_rxeof_xfr_wrap;
static funcptr ndis_rxeof_xfr_done_wrap;
static funcptr ndis_linksts_wrap;
static funcptr ndis_linksts_done_wrap;
static funcptr ndis_ticktask_wrap;
static funcptr ndis_starttask_wrap;
static funcptr ndis_resettask_wrap;
static funcptr ndis_inputtask_wrap;

static void ndis_tick		(void *);
static void ndis_ticktask	(device_object *, void *);
static void ndis_start		(struct ifnet *);
static void ndis_starttask	(device_object *, void *);
static void ndis_resettask	(device_object *, void *);
static void ndis_inputtask	(device_object *, void *);
static int ndis_ioctl		(struct ifnet *, u_long, caddr_t);
#if __FreeBSD_version < 700046
static int ndis_wi_ioctl_get	(struct ifnet *, u_long, caddr_t);
static int ndis_wi_ioctl_set	(struct ifnet *, u_long, caddr_t);
#endif
static int ndis_80211_ioctl_get	(struct ifnet *, u_long, caddr_t);
static int ndis_80211_ioctl_set	(struct ifnet *, u_long, caddr_t);
static int ndis_newstate	(struct ieee80211com *, enum ieee80211_state,
	int);
static void ndis_scan		(void *, int);
static void ndis_scan_results	(struct ndis_softc *);
static void ndis_scan_start	(struct ieee80211com *);
static void ndis_scan_end	(struct ieee80211com *);
static void ndis_set_channel	(struct ieee80211com *);
static void ndis_scan_curchan	(struct ieee80211com *, unsigned long);
static void ndis_scan_mindwell	(struct ieee80211com *);
static void ndis_init		(void *);
static void ndis_stop		(struct ndis_softc *);
static void ndis_watchdog	(struct ifnet *);
static int ndis_ifmedia_upd	(struct ifnet *);
static void ndis_ifmedia_sts	(struct ifnet *, struct ifmediareq *);
static int ndis_get_assoc	(struct ndis_softc *, ndis_wlan_bssid_ex **);
static int ndis_probe_offload	(struct ndis_softc *);
static int ndis_set_offload	(struct ndis_softc *);
static void ndis_getstate_80211	(struct ndis_softc *);
static void ndis_setstate_80211	(struct ndis_softc *);
#ifdef IEEE80211_F_WPA
static int ndis_set_cipher	(struct ndis_softc *, int);
static int ndis_set_wpa		(struct ndis_softc *);
static int ndis_add_key		(struct ieee80211com *,
	const struct ieee80211_key *, const u_int8_t []);
static int ndis_del_key		(struct ieee80211com *,
	const struct ieee80211_key *);
#endif
static void ndis_media_status	(struct ifnet *, struct ifmediareq *);

static void ndis_setmulti	(struct ndis_softc *);
static void ndis_map_sclist	(void *, bus_dma_segment_t *,
	int, bus_size_t, int);

static int ndisdrv_loaded = 0;

/*
 * This routine should call windrv_load() once for each driver
 * image. This will do the relocation and dynalinking for the
 * image, and create a Windows driver object which will be
 * saved in our driver database.
 */

int
ndisdrv_modevent(mod, cmd, arg)
	module_t		mod;
	int			cmd;
	void			*arg;
{
	int			error = 0;

	switch (cmd) {
	case MOD_LOAD:
		ndisdrv_loaded++;
                if (ndisdrv_loaded > 1)
			break;
		windrv_wrap((funcptr)ndis_rxeof, &ndis_rxeof_wrap,
		    3, WINDRV_WRAP_STDCALL);
		windrv_wrap((funcptr)ndis_rxeof_eth, &ndis_rxeof_eth_wrap,
		    8, WINDRV_WRAP_STDCALL);
		windrv_wrap((funcptr)ndis_rxeof_done, &ndis_rxeof_done_wrap,
		    1, WINDRV_WRAP_STDCALL);
		windrv_wrap((funcptr)ndis_rxeof_xfr, &ndis_rxeof_xfr_wrap,
		    4, WINDRV_WRAP_STDCALL);
		windrv_wrap((funcptr)ndis_rxeof_xfr_done,
		    &ndis_rxeof_xfr_done_wrap, 4, WINDRV_WRAP_STDCALL);
		windrv_wrap((funcptr)ndis_txeof, &ndis_txeof_wrap,
		    3, WINDRV_WRAP_STDCALL);
		windrv_wrap((funcptr)ndis_linksts, &ndis_linksts_wrap,
		    4, WINDRV_WRAP_STDCALL);
		windrv_wrap((funcptr)ndis_linksts_done,
		    &ndis_linksts_done_wrap, 1, WINDRV_WRAP_STDCALL);
		windrv_wrap((funcptr)ndis_ticktask, &ndis_ticktask_wrap,
		    2, WINDRV_WRAP_STDCALL);
		windrv_wrap((funcptr)ndis_starttask, &ndis_starttask_wrap,
		    2, WINDRV_WRAP_STDCALL);
		windrv_wrap((funcptr)ndis_resettask, &ndis_resettask_wrap,
		    2, WINDRV_WRAP_STDCALL);
		windrv_wrap((funcptr)ndis_inputtask, &ndis_inputtask_wrap,
		    2, WINDRV_WRAP_STDCALL);
		break;
	case MOD_UNLOAD:
		ndisdrv_loaded--;
		if (ndisdrv_loaded > 0)
			break;
		/* fallthrough */
	case MOD_SHUTDOWN:
		windrv_unwrap(ndis_rxeof_wrap);
		windrv_unwrap(ndis_rxeof_eth_wrap);
		windrv_unwrap(ndis_rxeof_done_wrap);
		windrv_unwrap(ndis_rxeof_xfr_wrap);
		windrv_unwrap(ndis_rxeof_xfr_done_wrap);
		windrv_unwrap(ndis_txeof_wrap);
		windrv_unwrap(ndis_linksts_wrap);
		windrv_unwrap(ndis_linksts_done_wrap);
		windrv_unwrap(ndis_ticktask_wrap);
		windrv_unwrap(ndis_starttask_wrap);
		windrv_unwrap(ndis_resettask_wrap);
		windrv_unwrap(ndis_inputtask_wrap);
		break;
	default:
		error = EINVAL;
		break;
	}

	return (error);
}

/*
 * Program the 64-bit multicast hash filter.
 */
static void
ndis_setmulti(sc)
	struct ndis_softc	*sc;
{
	struct ifnet		*ifp;
	struct ifmultiaddr	*ifma;
	int			len, mclistsz, error;
	uint8_t			*mclist;

	ifp = sc->ifp;

	if (!NDIS_INITIALIZED(sc))
		return;

	if (ifp->if_flags & IFF_ALLMULTI || ifp->if_flags & IFF_PROMISC) {
		sc->ndis_filter |= NDIS_PACKET_TYPE_ALL_MULTICAST;
		len = sizeof(sc->ndis_filter);
		error = ndis_set_info(sc, OID_GEN_CURRENT_PACKET_FILTER,
		    &sc->ndis_filter, &len);
		if (error)
			device_printf (sc->ndis_dev,
			    "set allmulti failed: %d\n", error);
		return;
	}

	if (TAILQ_EMPTY(&ifp->if_multiaddrs))
		return;

	len = sizeof(mclistsz);
	ndis_get_info(sc, OID_802_3_MAXIMUM_LIST_SIZE, &mclistsz, &len);

	mclist = malloc(ETHER_ADDR_LEN * mclistsz, M_TEMP, M_NOWAIT|M_ZERO);

	if (mclist == NULL) {
		sc->ndis_filter |= NDIS_PACKET_TYPE_ALL_MULTICAST;
		goto out;
	}

	sc->ndis_filter |= NDIS_PACKET_TYPE_MULTICAST;

	len = 0;
	IF_ADDR_LOCK(ifp);
	TAILQ_FOREACH(ifma, &ifp->if_multiaddrs, ifma_link) {
		if (ifma->ifma_addr->sa_family != AF_LINK)
			continue;
		bcopy(LLADDR((struct sockaddr_dl *)ifma->ifma_addr),
		    mclist + (ETHER_ADDR_LEN * len), ETHER_ADDR_LEN);
		len++;
		if (len > mclistsz) {
			IF_ADDR_UNLOCK(ifp);
			sc->ndis_filter |= NDIS_PACKET_TYPE_ALL_MULTICAST;
			sc->ndis_filter &= ~NDIS_PACKET_TYPE_MULTICAST;
			goto out;
		}
	}
	IF_ADDR_UNLOCK(ifp);

	len = len * ETHER_ADDR_LEN;
	error = ndis_set_info(sc, OID_802_3_MULTICAST_LIST, mclist, &len);
	if (error) {
		device_printf (sc->ndis_dev, "set mclist failed: %d\n", error);
		sc->ndis_filter |= NDIS_PACKET_TYPE_ALL_MULTICAST;
		sc->ndis_filter &= ~NDIS_PACKET_TYPE_MULTICAST;
	}

out:
	free(mclist, M_TEMP);

	len = sizeof(sc->ndis_filter);
	error = ndis_set_info(sc, OID_GEN_CURRENT_PACKET_FILTER,
	    &sc->ndis_filter, &len);
	if (error)
		device_printf (sc->ndis_dev, "set multi failed: %d\n", error);

	return;
}

static int
ndis_set_offload(sc)
	struct ndis_softc	*sc;
{
	ndis_task_offload	*nto;
	ndis_task_offload_hdr	*ntoh;
	ndis_task_tcpip_csum	*nttc;
	struct ifnet		*ifp;
	int			len, error;

	ifp = sc->ifp;

	if (!NDIS_INITIALIZED(sc))
		return(EINVAL);

	/* See if there's anything to set. */

	error = ndis_probe_offload(sc);
	if (error)
		return(error);
		
	if (sc->ndis_hwassist == 0 && ifp->if_capabilities == 0)
		return(0);

	len = sizeof(ndis_task_offload_hdr) + sizeof(ndis_task_offload) +
	    sizeof(ndis_task_tcpip_csum);

	ntoh = malloc(len, M_TEMP, M_NOWAIT|M_ZERO);

	if (ntoh == NULL)
		return(ENOMEM);

	ntoh->ntoh_vers = NDIS_TASK_OFFLOAD_VERSION;
	ntoh->ntoh_len = sizeof(ndis_task_offload_hdr);
	ntoh->ntoh_offset_firsttask = sizeof(ndis_task_offload_hdr);
	ntoh->ntoh_encapfmt.nef_encaphdrlen = sizeof(struct ether_header);
	ntoh->ntoh_encapfmt.nef_encap = NDIS_ENCAP_IEEE802_3;
	ntoh->ntoh_encapfmt.nef_flags = NDIS_ENCAPFLAG_FIXEDHDRLEN;

	nto = (ndis_task_offload *)((char *)ntoh +
	    ntoh->ntoh_offset_firsttask);

	nto->nto_vers = NDIS_TASK_OFFLOAD_VERSION;
	nto->nto_len = sizeof(ndis_task_offload);
	nto->nto_task = NDIS_TASK_TCPIP_CSUM;
	nto->nto_offset_nexttask = 0;
	nto->nto_taskbuflen = sizeof(ndis_task_tcpip_csum);

	nttc = (ndis_task_tcpip_csum *)nto->nto_taskbuf;

	if (ifp->if_capenable & IFCAP_TXCSUM)
		nttc->nttc_v4tx = sc->ndis_v4tx;

	if (ifp->if_capenable & IFCAP_RXCSUM)
		nttc->nttc_v4rx = sc->ndis_v4rx;

	error = ndis_set_info(sc, OID_TCP_TASK_OFFLOAD, ntoh, &len);
	free(ntoh, M_TEMP);

	return(error);
}

static int
ndis_probe_offload(sc)
	struct ndis_softc	*sc;
{
	ndis_task_offload	*nto;
	ndis_task_offload_hdr	*ntoh;
	ndis_task_tcpip_csum	*nttc = NULL;
	struct ifnet		*ifp;
	int			len, error, dummy;

	ifp = sc->ifp;

	len = sizeof(dummy);
	error = ndis_get_info(sc, OID_TCP_TASK_OFFLOAD, &dummy, &len);

	if (error != ENOSPC)
		return(error);

	ntoh = malloc(len, M_TEMP, M_NOWAIT|M_ZERO);

	if (ntoh == NULL)
		return(ENOMEM);

	ntoh->ntoh_vers = NDIS_TASK_OFFLOAD_VERSION;
	ntoh->ntoh_len = sizeof(ndis_task_offload_hdr);
	ntoh->ntoh_encapfmt.nef_encaphdrlen = sizeof(struct ether_header);
	ntoh->ntoh_encapfmt.nef_encap = NDIS_ENCAP_IEEE802_3;
	ntoh->ntoh_encapfmt.nef_flags = NDIS_ENCAPFLAG_FIXEDHDRLEN;

	error = ndis_get_info(sc, OID_TCP_TASK_OFFLOAD, ntoh, &len);

	if (error) {
		free(ntoh, M_TEMP);
		return(error);
	}

	if (ntoh->ntoh_vers != NDIS_TASK_OFFLOAD_VERSION) {
		free(ntoh, M_TEMP);
		return(EINVAL);
	}

	nto = (ndis_task_offload *)((char *)ntoh +
	    ntoh->ntoh_offset_firsttask);

	while (1) {
		switch (nto->nto_task) {
		case NDIS_TASK_TCPIP_CSUM:
			nttc = (ndis_task_tcpip_csum *)nto->nto_taskbuf;
			break;
		/* Don't handle these yet. */
		case NDIS_TASK_IPSEC:
		case NDIS_TASK_TCP_LARGESEND:
		default:
			break;
		}
		if (nto->nto_offset_nexttask == 0)
			break;
		nto = (ndis_task_offload *)((char *)nto +
		    nto->nto_offset_nexttask);
	}

	if (nttc == NULL) {
		free(ntoh, M_TEMP);
		return(ENOENT);
	}

	sc->ndis_v4tx = nttc->nttc_v4tx;
	sc->ndis_v4rx = nttc->nttc_v4rx;

	if (nttc->nttc_v4tx & NDIS_TCPSUM_FLAGS_IP_CSUM)
		sc->ndis_hwassist |= CSUM_IP;
	if (nttc->nttc_v4tx & NDIS_TCPSUM_FLAGS_TCP_CSUM)
		sc->ndis_hwassist |= CSUM_TCP;
	if (nttc->nttc_v4tx & NDIS_TCPSUM_FLAGS_UDP_CSUM)
		sc->ndis_hwassist |= CSUM_UDP;

	if (sc->ndis_hwassist)
		ifp->if_capabilities |= IFCAP_TXCSUM;

	if (nttc->nttc_v4rx & NDIS_TCPSUM_FLAGS_IP_CSUM)
		ifp->if_capabilities |= IFCAP_RXCSUM;
	if (nttc->nttc_v4rx & NDIS_TCPSUM_FLAGS_TCP_CSUM)
		ifp->if_capabilities |= IFCAP_RXCSUM;
	if (nttc->nttc_v4rx & NDIS_TCPSUM_FLAGS_UDP_CSUM)
		ifp->if_capabilities |= IFCAP_RXCSUM;

	free(ntoh, M_TEMP);
	return(0);
}

/*
 * Attach the interface. Allocate softc structures, do ifmedia
 * setup and ethernet/BPF attach.
 */
int
ndis_attach(dev)
	device_t		dev;
{
	u_char			eaddr[ETHER_ADDR_LEN];
	struct ndis_softc	*sc;
	driver_object		*pdrv;
	device_object		*pdo;
	struct ifnet		*ifp = NULL;
	int			error = 0, len, bands = 0;
	int			i;

	sc = device_get_softc(dev);
#if __FreeBSD_version < 600031
	sc->ifp = &sc->arpcom.ac_if;
	ifp = sc->ifp;
#else
	ifp = sc->ifp = if_alloc(IFT_ETHER);
	if (ifp == NULL) {
		error = ENOSPC;
		goto fail;
	}
#endif
	ifp->if_softc = sc;

	KeInitializeSpinLock(&sc->ndis_spinlock);
	KeInitializeSpinLock(&sc->ndis_rxlock);
	InitializeListHead(&sc->ndis_shlist);

	if (sc->ndis_iftype == PCMCIABus) {
		error = ndis_alloc_amem(sc);
		if (error) {
			device_printf(dev, "failed to allocate "
			    "attribute memory\n");
			goto fail;
		}
	}

#if __FreeBSD_version < 502113
	sysctl_ctx_init(&sc->ndis_ctx);
#endif

	/* Create sysctl registry nodes */
	ndis_create_sysctls(sc);

	/* Find the PDO for this device instance. */

	if (sc->ndis_iftype == PCIBus)
		pdrv = windrv_lookup(0, "PCI Bus");
	else if (sc->ndis_iftype == PCMCIABus)
		pdrv = windrv_lookup(0, "PCCARD Bus");
	else
		pdrv = windrv_lookup(0, "USB Bus");
	pdo = windrv_find_pdo(pdrv, dev);

	/*
	 * Create a new functional device object for this
	 * device. This is what creates the miniport block
	 * for this device instance.
	 */

	if (NdisAddDevice(sc->ndis_dobj, pdo) != STATUS_SUCCESS) {
		device_printf(dev, "failed to create FDO!\n");
		error = ENXIO;
		goto fail;
	}

	/* Tell the user what version of the API the driver is using. */
	device_printf(dev, "NDIS API version: %d.%d\n",
	    sc->ndis_chars->nmc_version_major,
	    sc->ndis_chars->nmc_version_minor);

	/* Do resource conversion. */
	if (sc->ndis_iftype == PCMCIABus || sc->ndis_iftype == PCIBus)
		ndis_convert_res(sc);
	else
		sc->ndis_block->nmb_rlist = NULL;

	/* Install our RX and TX interrupt handlers. */
	sc->ndis_block->nmb_senddone_func = ndis_txeof_wrap;
	sc->ndis_block->nmb_pktind_func = ndis_rxeof_wrap;
	sc->ndis_block->nmb_ethrxindicate_func = ndis_rxeof_eth_wrap;
	sc->ndis_block->nmb_ethrxdone_func = ndis_rxeof_done_wrap;
	sc->ndis_block->nmb_tdcond_func = ndis_rxeof_xfr_done_wrap;

	/* Override the status handler so we can detect link changes. */
	sc->ndis_block->nmb_status_func = ndis_linksts_wrap;
	sc->ndis_block->nmb_statusdone_func = ndis_linksts_done_wrap;

	/* Set up work item handlers. */
	sc->ndis_tickitem = IoAllocateWorkItem(sc->ndis_block->nmb_deviceobj);
	sc->ndis_startitem = IoAllocateWorkItem(sc->ndis_block->nmb_deviceobj);
	sc->ndis_resetitem = IoAllocateWorkItem(sc->ndis_block->nmb_deviceobj);
	sc->ndis_inputitem = IoAllocateWorkItem(sc->ndis_block->nmb_deviceobj);
	KeInitializeDpc(&sc->ndis_rxdpc, ndis_rxeof_xfr_wrap, sc->ndis_block);

        /* make sure drv flags are all cleared before initing the NIC. */

        ifp->if_drv_flags = 0;

	/* Call driver's init routine. */
	if (ndis_init_nic(sc)) {
		device_printf (dev, "init handler failed\n");
		error = ENXIO;
		goto fail;
	}

	/*
	 * Get station address from the driver.
	 */
	len = sizeof(eaddr);
	ndis_get_info(sc, OID_802_3_CURRENT_ADDRESS, &eaddr, &len);

	/*
	 * Figure out how big to make the TX buffer pool.
	 */

	len = sizeof(sc->ndis_maxpkts);
	if (ndis_get_info(sc, OID_GEN_MAXIMUM_SEND_PACKETS,
		    &sc->ndis_maxpkts, &len)) {
		device_printf (dev, "failed to get max TX packets\n");
		error = ENXIO;
		goto fail;
	}

	/*
	 * If this is a deserialized miniport, we don't have
	 * to honor the OID_GEN_MAXIMUM_SEND_PACKETS result.
	 */

	if (!NDIS_SERIALIZED(sc->ndis_block))
		sc->ndis_maxpkts = NDIS_TXPKTS;

	/* Enforce some sanity, just in case. */

	if (sc->ndis_maxpkts == 0)
		sc->ndis_maxpkts = 10;

	sc->ndis_txarray = malloc(sizeof(ndis_packet *) *
	    NDIS_TXPKTS, M_DEVBUF, M_NOWAIT|M_ZERO);

	/* Allocate a pool of ndis_packets for TX encapsulation. */

	NdisAllocatePacketPool(&i, &sc->ndis_txpool,
	   NDIS_TXPKTS, PROTOCOL_RESERVED_SIZE_IN_PACKET);

	if (i != NDIS_STATUS_SUCCESS) {
		sc->ndis_txpool = NULL;
		device_printf(dev, "failed to allocate TX packet pool");
		error = ENOMEM;
		goto fail;
	}

	sc->ndis_txpending = sc->ndis_maxpkts;

	sc->ndis_oidcnt = 0;
	/* Get supported oid list. */
	ndis_get_supported_oids(sc, &sc->ndis_oids, &sc->ndis_oidcnt);

	/* If the NDIS module requested scatter/gather, init maps. */
	if (sc->ndis_sc)
		ndis_init_dma(sc);

	/*
	 * See if the OID_802_11_CONFIGURATION OID is
	 * supported by this driver. If it is, then this an 802.11
	 * wireless driver, and we should set up media for wireless.
	 */
	for (i = 0; i < sc->ndis_oidcnt; i++) {
		if (sc->ndis_oids[i] == OID_802_11_CONFIGURATION) {
			sc->ndis_80211++;
			break;
		}
	}

	/* Check for task offload support. */
	ndis_probe_offload(sc);

#if __FreeBSD_version < 502109
	/*
	 * An NDIS device was detected. Inform the world.
	 */
	device_printf(dev, "%s address: %6D\n",
	    sc->ndis_80211 ? "802.11" : "Ethernet", eaddr, ":");
#endif

	if_initname(ifp, device_get_name(dev), device_get_unit(dev));
	ifp->if_mtu = ETHERMTU;
	ifp->if_flags = IFF_BROADCAST | IFF_SIMPLEX | IFF_MULTICAST;
	ifp->if_ioctl = ndis_ioctl;
	ifp->if_start = ndis_start;
	ifp->if_watchdog = ndis_watchdog;
	ifp->if_init = ndis_init;
	ifp->if_baudrate = 10000000;
#if __FreeBSD_version < 502114
	ifp->if_snd.ifq_maxlen = 50;
#else
	IFQ_SET_MAXLEN(&ifp->if_snd, 50);
	ifp->if_snd.ifq_drv_maxlen = 25;
	IFQ_SET_READY(&ifp->if_snd);
#endif
	ifp->if_capenable = ifp->if_capabilities;
	ifp->if_hwassist = sc->ndis_hwassist;

	/* Do media setup */
	if (sc->ndis_80211) {
		struct ieee80211com	*ic = (void *)&sc->ic;
		ndis_80211_rates_ex	rates;
		struct ndis_80211_nettype_list *ntl;
		uint32_t		arg;
		int			r;

#if __FreeBSD_version >= 700000
		sc->ndis_tq = taskqueue_create("nids_taskq", M_NOWAIT | M_ZERO,
		    taskqueue_thread_enqueue, &sc->ndis_tq);
		taskqueue_start_threads(&sc->ndis_tq, 1, PI_NET, "%s taskq",
		    device_get_nameunit(dev));
#else
		sc->ndis_tq = taskqueue_create("ndis_taskq", M_NOWAIT | M_ZERO,
		    taskqueue_thread_enqueue, &sc->ndis_tq, &sc->sc_tqproc);
		kthread_create(taskqueue_thread_loop, &sc->ndis_tq, &sc->sc_tqproc,
		    0, 0, "%s taskq", device_get_nameunit(dev));
#endif
		TASK_INIT(&sc->ndis_scantask, 0, ndis_scan, sc);

#if __FreeBSD_version >= 600007
		ic->ic_ifp = ifp;
#endif
	        ic->ic_phytype = IEEE80211_T_DS;
		ic->ic_opmode = IEEE80211_M_STA;
		ic->ic_caps = IEEE80211_C_IBSS;
		setbit(ic->ic_modecaps, IEEE80211_MODE_AUTO);
		len = 0;
		r = ndis_get_info(sc, OID_802_11_NETWORK_TYPES_SUPPORTED,
		    NULL, &len);
		if (r != ENOSPC)
			goto nonettypes;
		ntl = malloc(len, M_DEVBUF, M_NOWAIT|M_ZERO);
		r = ndis_get_info(sc, OID_802_11_NETWORK_TYPES_SUPPORTED,
		    ntl, &len);
		if (r != 0) {
			free(ntl, M_DEVBUF);
			goto nonettypes;
		}

		for (i = 0; i < ntl->ntl_items; i++) {
			switch (ntl->ntl_type[i]) {
			case NDIS_80211_NETTYPE_11FH:
			case NDIS_80211_NETTYPE_11DS:
				setbit(ic->ic_modecaps, IEEE80211_MODE_11B);
				setbit(&bands, IEEE80211_MODE_11B);
				break;
			case NDIS_80211_NETTYPE_11OFDM5:
				setbit(ic->ic_modecaps, IEEE80211_MODE_11A);
				setbit(&bands, IEEE80211_MODE_11A);
				break;
			case NDIS_80211_NETTYPE_11OFDM24:
				setbit(ic->ic_modecaps, IEEE80211_MODE_11G);
				setbit(&bands, IEEE80211_MODE_11G);
				break;
			default:
				break;
			}
		}
		free(ntl, M_DEVBUF);
nonettypes:
		len = sizeof(rates);
		bzero((char *)&rates, len);
		r = ndis_get_info(sc, OID_802_11_SUPPORTED_RATES,
		    (void *)rates, &len);
		if (r)
			device_printf (dev, "get rates failed: 0x%x\n", r);
		/*
		 * Since the supported rates only up to 8 can be supported,
		 * if this is not 802.11b we're just going to be faking it
		 * all up to heck.
		 */

#define TESTSETRATE(x, y)						\
	do {								\
		int			i;				\
		for (i = 0; i < ic->ic_sup_rates[x].rs_nrates; i++) {	\
			if (ic->ic_sup_rates[x].rs_rates[i] == (y))	\
				break;					\
		}							\
		if (i == ic->ic_sup_rates[x].rs_nrates) {		\
			ic->ic_sup_rates[x].rs_rates[i] = (y);		\
			ic->ic_sup_rates[x].rs_nrates++;		\
		}							\
	} while (0)

#define SETRATE(x, y)	\
	ic->ic_sup_rates[x].rs_rates[ic->ic_sup_rates[x].rs_nrates] = (y)
#define INCRATE(x)	\
	ic->ic_sup_rates[x].rs_nrates++

		ic->ic_curmode = IEEE80211_MODE_AUTO;
		if (isset(ic->ic_modecaps, IEEE80211_MODE_11A))
			ic->ic_sup_rates[IEEE80211_MODE_11A].rs_nrates = 0;
		if (isset(ic->ic_modecaps, IEEE80211_MODE_11B))
			ic->ic_sup_rates[IEEE80211_MODE_11B].rs_nrates = 0;
		if (isset(ic->ic_modecaps, IEEE80211_MODE_11G))
			ic->ic_sup_rates[IEEE80211_MODE_11G].rs_nrates = 0;
		for (i = 0; i < len; i++) {
			switch (rates[i] & IEEE80211_RATE_VAL) {
			case 2:
			case 4:
			case 11:
			case 10:
			case 22:
				if (isclr(ic->ic_modecaps, IEEE80211_MODE_11B)) {
					/* Lazy-init 802.11b. */
					setbit(ic->ic_modecaps,
					    IEEE80211_MODE_11B);
					ic->ic_sup_rates[IEEE80211_MODE_11B].
					    rs_nrates = 0;
				}
				SETRATE(IEEE80211_MODE_11B, rates[i]);
				INCRATE(IEEE80211_MODE_11B);
				break;
			default:
				if (isset(ic->ic_modecaps, IEEE80211_MODE_11A)) {
					SETRATE(IEEE80211_MODE_11A, rates[i]);
					INCRATE(IEEE80211_MODE_11A);
				}
				if (isset(ic->ic_modecaps, IEEE80211_MODE_11G)) {
					SETRATE(IEEE80211_MODE_11G, rates[i]);
					INCRATE(IEEE80211_MODE_11G);
				}
				break;
			}
		}

		/*
		 * If the hardware supports 802.11g, it most
		 * likely supports 802.11b and all of the
		 * 802.11b and 802.11g speeds, so maybe we can
		 * just cheat here.  Just how in the heck do
		 * we detect turbo modes, though?
		 */
		if (isset(ic->ic_modecaps, IEEE80211_MODE_11B)) {
			TESTSETRATE(IEEE80211_MODE_11B,
			    IEEE80211_RATE_BASIC|2);
			TESTSETRATE(IEEE80211_MODE_11B,
			    IEEE80211_RATE_BASIC|4);
			TESTSETRATE(IEEE80211_MODE_11B,
			    IEEE80211_RATE_BASIC|11);
			TESTSETRATE(IEEE80211_MODE_11B,
			    IEEE80211_RATE_BASIC|22);
		}
		if (isset(ic->ic_modecaps, IEEE80211_MODE_11G)) {
			TESTSETRATE(IEEE80211_MODE_11G, 47);
			TESTSETRATE(IEEE80211_MODE_11G, 72);
			TESTSETRATE(IEEE80211_MODE_11G, 96);
			TESTSETRATE(IEEE80211_MODE_11G, 108);
		}
		if (isset(ic->ic_modecaps, IEEE80211_MODE_11A)) {
			TESTSETRATE(IEEE80211_MODE_11A, 47);
			TESTSETRATE(IEEE80211_MODE_11A, 72);
			TESTSETRATE(IEEE80211_MODE_11A, 96);
			TESTSETRATE(IEEE80211_MODE_11A, 108);
		}
#undef SETRATE
#undef INCRATE
		ieee80211_init_channels(ic, 0, CTRY_DEFAULT, bands, 0, 1);

		/*
		 * To test for WPA support, we need to see if we can
		 * set AUTHENTICATION_MODE to WPA and read it back
		 * successfully.
		 */
#ifdef IEEE80211_F_WPA
		i = sizeof(arg);
		arg = NDIS_80211_AUTHMODE_WPA;
		r = ndis_set_info(sc,
		    OID_802_11_AUTHENTICATION_MODE, &arg, &i);
		if (r == 0) {
			r = ndis_get_info(sc,
			    OID_802_11_AUTHENTICATION_MODE, &arg, &i);
			if (r == 0 && arg == NDIS_80211_AUTHMODE_WPA)
				ic->ic_caps |= IEEE80211_C_WPA;
		}

		/*
		 * To test for supported ciphers, we set each
		 * available encryption type in descending order.
		 * If ENC3 works, then we have WEP, TKIP and AES.
		 * If only ENC2 works, then we have WEP and TKIP.
		 * If only ENC1 works, then we have just WEP.
		 */
		i = sizeof(arg);
		arg = NDIS_80211_WEPSTAT_ENC3ENABLED;
		r = ndis_set_info(sc, OID_802_11_ENCRYPTION_STATUS, &arg, &i);
		if (r == 0) {
			ic->ic_caps |= IEEE80211_C_WEP|IEEE80211_C_TKIP|
			    IEEE80211_C_AES_CCM;
			goto got_crypto;
		}
		arg = NDIS_80211_WEPSTAT_ENC2ENABLED;
		r = ndis_set_info(sc, OID_802_11_ENCRYPTION_STATUS, &arg, &i);
		if (r == 0) {
			ic->ic_caps |= IEEE80211_C_WEP|IEEE80211_C_TKIP;
			goto got_crypto;
		}
#endif
		arg = NDIS_80211_WEPSTAT_ENC1ENABLED;
		r = ndis_set_info(sc, OID_802_11_ENCRYPTION_STATUS, &arg, &i);
		if (r == 0)
			ic->ic_caps |= IEEE80211_C_WEP;
#ifdef IEEE80211_F_WPA
got_crypto:
#endif
		i = sizeof(arg);
		r = ndis_get_info(sc, OID_802_11_POWER_MODE, &arg, &i);
		if (r == 0)
			ic->ic_caps |= IEEE80211_C_PMGT;
		bcopy(eaddr, &ic->ic_myaddr, sizeof(eaddr));
#if __FreeBSD_version < 600007
		ieee80211_ifattach(ifp);
		ieee80211_media_init(ifp, ieee80211_media_change,
		    ndis_media_status);
#else
		ieee80211_ifattach(ic);
		ieee80211_media_init(ic, ieee80211_media_change,
		    ndis_media_status);
#endif
		ic->ic_scan_start = ndis_scan_start;
		ic->ic_scan_end = ndis_scan_end;
		ic->ic_set_channel = ndis_set_channel;
		ic->ic_scan_curchan = ndis_scan_curchan;
		ic->ic_scan_mindwell = ndis_scan_mindwell;
		ic->ic_bsschan = IEEE80211_CHAN_ANYC;
		ic->ic_bss->ni_chan = ic->ic_bsschan;
		/* override state transition machine */
		sc->ndis_newstate = ic->ic_newstate;
		ic->ic_newstate = ndis_newstate;
#ifdef IEEE80211_F_WPA
		/* install key handing routines */
		ic->ic_crypto.cs_key_set = ndis_add_key;
		ic->ic_crypto.cs_key_delete = ndis_del_key;
#endif
	} else {
		ifmedia_init(&sc->ifmedia, IFM_IMASK, ndis_ifmedia_upd,
		    ndis_ifmedia_sts);
		ifmedia_add(&sc->ifmedia, IFM_ETHER|IFM_10_T, 0, NULL);
		ifmedia_add(&sc->ifmedia, IFM_ETHER|IFM_10_T|IFM_FDX, 0, NULL);
		ifmedia_add(&sc->ifmedia, IFM_ETHER|IFM_100_TX, 0, NULL);
		ifmedia_add(&sc->ifmedia,
		    IFM_ETHER|IFM_100_TX|IFM_FDX, 0, NULL);
		ifmedia_add(&sc->ifmedia, IFM_ETHER|IFM_AUTO, 0, NULL);
		ifmedia_set(&sc->ifmedia, IFM_ETHER|IFM_AUTO);
		ether_ifattach(ifp, eaddr);
	}

fail:
	if (error)
		ndis_detach(dev);
	else
		/* We're done talking to the NIC for now; halt it. */
		ndis_halt_nic(sc);

	return(error);
}

/*
 * Shutdown hardware and free up resources. This can be called any
 * time after the mutex has been initialized. It is called in both
 * the error case in attach and the normal detach case so it needs
 * to be careful about only freeing resources that have actually been
 * allocated.
 */
int
ndis_detach(dev)
	device_t		dev;
{
	struct ndis_softc	*sc;
	struct ifnet		*ifp;
	driver_object		*drv;

	sc = device_get_softc(dev);
	NDIS_LOCK(sc);
	ifp = sc->ifp;
	ifp->if_flags &= ~IFF_UP;

	if (device_is_attached(dev)) {
		NDIS_UNLOCK(sc);
		ndis_stop(sc);
		if (sc->ndis_80211)
#if __FreeBSD_version < 600007
			ieee80211_ifdetach(ifp);
#else
			ieee80211_ifdetach(&sc->ic);
#endif
		else
			ether_ifdetach(ifp);
	} else
		NDIS_UNLOCK(sc);

	if (sc->ndis_tickitem != NULL)
		IoFreeWorkItem(sc->ndis_tickitem);
	if (sc->ndis_startitem != NULL)
		IoFreeWorkItem(sc->ndis_startitem);
	if (sc->ndis_resetitem != NULL)
		IoFreeWorkItem(sc->ndis_resetitem);
	if (sc->ndis_inputitem != NULL)
		IoFreeWorkItem(sc->ndis_inputitem);

	bus_generic_detach(dev);

	if (sc->ndis_irq)
		bus_release_resource(dev, SYS_RES_IRQ, 0, sc->ndis_irq);
	if (sc->ndis_res_io)
		bus_release_resource(dev, SYS_RES_IOPORT,
		    sc->ndis_io_rid, sc->ndis_res_io);
	if (sc->ndis_res_mem)
		bus_release_resource(dev, SYS_RES_MEMORY,
		    sc->ndis_mem_rid, sc->ndis_res_mem);
	if (sc->ndis_res_altmem)
		bus_release_resource(dev, SYS_RES_MEMORY,
		    sc->ndis_altmem_rid, sc->ndis_res_altmem);

#if __FreeBSD_version >= 600031
	if (ifp != NULL)
		if_free(ifp);
#endif

	if (sc->ndis_iftype == PCMCIABus)
		ndis_free_amem(sc);

	if (sc->ndis_sc)
		ndis_destroy_dma(sc);

	if (sc->ndis_txarray)
		free(sc->ndis_txarray, M_DEVBUF);

	if (!sc->ndis_80211)
		ifmedia_removeall(&sc->ifmedia);

	if (sc->ndis_txpool != NULL)
		NdisFreePacketPool(sc->ndis_txpool);

	ndis_unload_driver(sc);

	/* Destroy the PDO for this device. */
	
	if (sc->ndis_iftype == PCIBus)
		drv = windrv_lookup(0, "PCI Bus");
	else if (sc->ndis_iftype == PCMCIABus)
		drv = windrv_lookup(0, "PCCARD Bus");
	else
		drv = windrv_lookup(0, "USB Bus");
	if (drv == NULL)
		panic("couldn't find driver object");
	windrv_destroy_pdo(drv, dev);

	if (sc->ndis_iftype == PCIBus)
		bus_dma_tag_destroy(sc->ndis_parent_tag);

	taskqueue_free(sc->ndis_tq);
#if __FreeBSD_version < 502113
	sysctl_ctx_free(&sc->ndis_ctx);
#endif

	return(0);
}

int
ndis_suspend(dev)
	device_t		dev;
{
	struct ndis_softc	*sc;
	struct ifnet		*ifp;

	sc = device_get_softc(dev);
	ifp = sc->ifp;

#ifdef notdef
	if (NDIS_INITIALIZED(sc))
        	ndis_stop(sc);
#endif

	return(0);
}

int
ndis_resume(dev)
	device_t		dev;
{
	struct ndis_softc	*sc;
	struct ifnet		*ifp;

	sc = device_get_softc(dev);
	ifp = sc->ifp;

	if (NDIS_INITIALIZED(sc))
        	ndis_init(sc);

	return(0);
}

/*
 * The following bunch of routines are here to support drivers that
 * use the NdisMEthIndicateReceive()/MiniportTransferData() mechanism.
 * The NdisMEthIndicateReceive() handler runs at DISPATCH_LEVEL for
 * serialized miniports, or IRQL <= DISPATCH_LEVEL for deserialized
 * miniports.
 */
 
static void
ndis_rxeof_eth(adapter, ctx, addr, hdr, hdrlen, lookahead, lookaheadlen, pktlen)
	ndis_handle		adapter;
	ndis_handle		ctx;
	char			*addr;
	void			*hdr;
	uint32_t		hdrlen;
	void			*lookahead;
	uint32_t		lookaheadlen;
	uint32_t		pktlen;
{
	ndis_miniport_block	*block;
	uint8_t			irql = 0;
	uint32_t		status;
	ndis_buffer		*b;
	ndis_packet		*p;
	struct mbuf		*m;
	ndis_ethpriv		*priv;

	block = adapter;

	m = m_getcl(M_DONTWAIT, MT_DATA, M_PKTHDR);

	if (m == NULL) {
		NdisFreePacket(p);
		return;
	}

	/* Save the data provided to us so far. */

	m->m_len = lookaheadlen + hdrlen;
	m->m_pkthdr.len = pktlen + hdrlen;
	m->m_next = NULL;
	m_copyback(m, 0, hdrlen, hdr);
	m_copyback(m, hdrlen, lookaheadlen, lookahead);

	/* Now create a fake NDIS_PACKET to hold the data */

	NdisAllocatePacket(&status, &p, block->nmb_rxpool);

	if (status != NDIS_STATUS_SUCCESS) {
		m_freem(m);
		return;
	}

	p->np_m0 = m;

	b = IoAllocateMdl(m->m_data, m->m_pkthdr.len, FALSE, FALSE, NULL);

	if (b == NULL) {
		NdisFreePacket(p);
		m_freem(m);
		return;
	}

	p->np_private.npp_head = p->np_private.npp_tail = b;
	p->np_private.npp_totlen = m->m_pkthdr.len;

	/* Save the packet RX context somewhere. */
	priv = (ndis_ethpriv *)&p->np_protocolreserved;
	priv->nep_ctx = ctx;

	if (!NDIS_SERIALIZED(block))
		KeAcquireSpinLock(&block->nmb_lock, &irql);

	InsertTailList((&block->nmb_packetlist), (&p->np_list));

	if (!NDIS_SERIALIZED(block))
		KeReleaseSpinLock(&block->nmb_lock, irql);

	return;
}

/*
 * NdisMEthIndicateReceiveComplete() handler, runs at DISPATCH_LEVEL
 * for serialized miniports, or IRQL <= DISPATCH_LEVEL for deserialized
 * miniports.
 */

static void
ndis_rxeof_done(adapter)
	ndis_handle		adapter;
{
	struct ndis_softc	*sc;
	ndis_miniport_block	*block;

	block = adapter;

	/* Schedule transfer/RX of queued packets. */

	sc = device_get_softc(block->nmb_physdeviceobj->do_devext);

	KeInsertQueueDpc(&sc->ndis_rxdpc, NULL, NULL);

	return;
}

/*
 * MiniportTransferData() handler, runs at DISPATCH_LEVEL.
 */
static void
ndis_rxeof_xfr(dpc, adapter, sysarg1, sysarg2)
	kdpc			*dpc;
	ndis_handle		adapter;
	void			*sysarg1;
	void			*sysarg2;
{
	ndis_miniport_block	*block;
	struct ndis_softc	*sc;
	ndis_packet		*p;
	list_entry		*l;
	uint32_t		status;
	ndis_ethpriv		*priv;
	struct ifnet		*ifp;
	struct mbuf		*m;

	block = adapter;
	sc = device_get_softc(block->nmb_physdeviceobj->do_devext);
	ifp = sc->ifp;

	KeAcquireSpinLockAtDpcLevel(&block->nmb_lock);

	l = block->nmb_packetlist.nle_flink;
	while(!IsListEmpty(&block->nmb_packetlist)) {
		l = RemoveHeadList((&block->nmb_packetlist));
		p = CONTAINING_RECORD(l, ndis_packet, np_list);
		InitializeListHead((&p->np_list));

		priv = (ndis_ethpriv *)&p->np_protocolreserved;
		m = p->np_m0;
		p->np_softc = sc;
		p->np_m0 = NULL;

		KeReleaseSpinLockFromDpcLevel(&block->nmb_lock);

		status = MSCALL6(sc->ndis_chars->nmc_transferdata_func,
		    p, &p->np_private.npp_totlen, block, priv->nep_ctx,
		    m->m_len, m->m_pkthdr.len - m->m_len);

		KeAcquireSpinLockAtDpcLevel(&block->nmb_lock);

		/*
		 * If status is NDIS_STATUS_PENDING, do nothing and
		 * wait for a callback to the ndis_rxeof_xfr_done()
		 * handler.
	 	 */

		m->m_len = m->m_pkthdr.len;
		m->m_pkthdr.rcvif = ifp;

		if (status == NDIS_STATUS_SUCCESS) {
			IoFreeMdl(p->np_private.npp_head);
			NdisFreePacket(p);
			KeAcquireSpinLockAtDpcLevel(&sc->ndis_rxlock);
			_IF_ENQUEUE(&sc->ndis_rxqueue, m);
			KeReleaseSpinLockFromDpcLevel(&sc->ndis_rxlock);
			IoQueueWorkItem(sc->ndis_inputitem,
			    (io_workitem_func)ndis_inputtask_wrap,
			    WORKQUEUE_CRITICAL, ifp);
		}

		if (status == NDIS_STATUS_FAILURE)
			m_freem(m);

		/* Advance to next packet */
		l = block->nmb_packetlist.nle_flink;
	}

	KeReleaseSpinLockFromDpcLevel(&block->nmb_lock);

	return;
}

/*
 * NdisMTransferDataComplete() handler, runs at DISPATCH_LEVEL.
 */
static void
ndis_rxeof_xfr_done(adapter, packet, status, len)
	ndis_handle		adapter;
	ndis_packet		*packet;
	uint32_t		status;
	uint32_t		len;
{
	ndis_miniport_block	*block;
	struct ndis_softc	*sc;
	struct ifnet		*ifp;
	struct mbuf		*m;

	block = adapter;
	sc = device_get_softc(block->nmb_physdeviceobj->do_devext);
	ifp = sc->ifp;

	m = packet->np_m0;
	IoFreeMdl(packet->np_private.npp_head);
	NdisFreePacket(packet);

	if (status != NDIS_STATUS_SUCCESS) {
		m_freem(m);
		return;
	}

	m->m_len = m->m_pkthdr.len;
	m->m_pkthdr.rcvif = ifp;
	KeAcquireSpinLockAtDpcLevel(&sc->ndis_rxlock);
	_IF_ENQUEUE(&sc->ndis_rxqueue, m);
	KeReleaseSpinLockFromDpcLevel(&sc->ndis_rxlock);
	IoQueueWorkItem(sc->ndis_inputitem,
	    (io_workitem_func)ndis_inputtask_wrap,
	    WORKQUEUE_CRITICAL, ifp);

	return;
}
/*
 * A frame has been uploaded: pass the resulting mbuf chain up to
 * the higher level protocols.
 *
 * When handling received NDIS packets, the 'status' field in the
 * out-of-band portion of the ndis_packet has special meaning. In the
 * most common case, the underlying NDIS driver will set this field
 * to NDIS_STATUS_SUCCESS, which indicates that it's ok for us to
 * take posession of it. We then change the status field to
 * NDIS_STATUS_PENDING to tell the driver that we now own the packet,
 * and that we will return it at some point in the future via the
 * return packet handler.
 *
 * If the driver hands us a packet with a status of NDIS_STATUS_RESOURCES,
 * this means the driver is running out of packet/buffer resources and
 * wants to maintain ownership of the packet. In this case, we have to
 * copy the packet data into local storage and let the driver keep the
 * packet.
 */
static void
ndis_rxeof(adapter, packets, pktcnt)
	ndis_handle		adapter;
	ndis_packet		**packets;
	uint32_t		pktcnt;
{
	struct ndis_softc	*sc;
	ndis_miniport_block	*block;
	ndis_packet		*p;
	uint32_t		s;
	ndis_tcpip_csum		*csum;
	struct ifnet		*ifp;
	struct mbuf		*m0, *m;
	int			i;

	block = (ndis_miniport_block *)adapter;
	sc = device_get_softc(block->nmb_physdeviceobj->do_devext);
	ifp = sc->ifp;

	/*
	 * There's a slim chance the driver may indicate some packets
	 * before we're completely ready to handle them. If we detect this,
	 * we need to return them to the miniport and ignore them.
	 */

        if (!(ifp->if_drv_flags & IFF_DRV_RUNNING)) {
		for (i = 0; i < pktcnt; i++) {
			p = packets[i];
			if (p->np_oob.npo_status == NDIS_STATUS_SUCCESS) {
				p->np_refcnt++;
				ndis_return_packet(p, block);
			}
		}
		return;
        }

	for (i = 0; i < pktcnt; i++) {
		p = packets[i];
		/* Stash the softc here so ptom can use it. */
		p->np_softc = sc;
		if (ndis_ptom(&m0, p)) {
			device_printf (sc->ndis_dev, "ptom failed\n");
			if (p->np_oob.npo_status == NDIS_STATUS_SUCCESS)
				ndis_return_packet(p, block);
		} else {
#ifdef notdef
			if (p->np_oob.npo_status == NDIS_STATUS_RESOURCES) {
				m = m_dup(m0, M_DONTWAIT);
				/*
				 * NOTE: we want to destroy the mbuf here, but
				 * we don't actually want to return it to the
				 * driver via the return packet handler. By
				 * bumping np_refcnt, we can prevent the
				 * ndis_return_packet() routine from actually
				 * doing anything.
				 */
				p->np_refcnt++;
				m_freem(m0);
				if (m == NULL)
					ifp->if_ierrors++;
				else
					m0 = m;
			} else
				p->np_oob.npo_status = NDIS_STATUS_PENDING;
#endif
			m = m_dup(m0, M_DONTWAIT);
			if (p->np_oob.npo_status == NDIS_STATUS_RESOURCES)
				p->np_refcnt++;
			else
				p->np_oob.npo_status = NDIS_STATUS_PENDING;
			m_freem(m0);
			if (m == NULL) {
				ifp->if_ierrors++;
				continue;
			}
			m0 = m;
			m0->m_pkthdr.rcvif = ifp;

			/* Deal with checksum offload. */

			if (ifp->if_capenable & IFCAP_RXCSUM &&
			    p->np_ext.npe_info[ndis_tcpipcsum_info] != NULL) {
				s = (uintptr_t)
			 	    p->np_ext.npe_info[ndis_tcpipcsum_info];
				csum = (ndis_tcpip_csum *)&s;
				if (csum->u.ntc_rxflags &
				    NDIS_RXCSUM_IP_PASSED)
					m0->m_pkthdr.csum_flags |=
					    CSUM_IP_CHECKED|CSUM_IP_VALID;
				if (csum->u.ntc_rxflags &
				    (NDIS_RXCSUM_TCP_PASSED |
				    NDIS_RXCSUM_UDP_PASSED)) {
					m0->m_pkthdr.csum_flags |=
					    CSUM_DATA_VALID|CSUM_PSEUDO_HDR;
					m0->m_pkthdr.csum_data = 0xFFFF;
				}
			}

			KeAcquireSpinLockAtDpcLevel(&sc->ndis_rxlock);
			_IF_ENQUEUE(&sc->ndis_rxqueue, m0);
			KeReleaseSpinLockFromDpcLevel(&sc->ndis_rxlock);
			IoQueueWorkItem(sc->ndis_inputitem,
			    (io_workitem_func)ndis_inputtask_wrap,
			    WORKQUEUE_CRITICAL, ifp);
		}
	}

	return;
}

/*
 * This routine is run at PASSIVE_LEVEL. We use this routine to pass
 * packets into the stack in order to avoid calling (*ifp->if_input)()
 * with any locks held (at DISPATCH_LEVEL, we'll be holding the
 * 'dispatch level' per-cpu sleep lock).
 */

static void
ndis_inputtask(dobj, arg)
	device_object		*dobj;
	void			*arg;
{
	ndis_miniport_block	*block;
	struct ifnet		*ifp;
	struct ndis_softc	*sc;
	struct mbuf		*m;
	uint8_t			irql;

	ifp = arg;
	sc = ifp->if_softc;
	block = dobj->do_devext;

	KeAcquireSpinLock(&sc->ndis_rxlock, &irql);
	while(1) {
		_IF_DEQUEUE(&sc->ndis_rxqueue, m);
		if (m == NULL)
			break;
		KeReleaseSpinLock(&sc->ndis_rxlock, irql);
		ifp->if_ipackets++;
		(*ifp->if_input)(ifp, m);
		KeAcquireSpinLock(&sc->ndis_rxlock, &irql);
	}
	KeReleaseSpinLock(&sc->ndis_rxlock, irql);

	return;
}

/*
 * A frame was downloaded to the chip. It's safe for us to clean up
 * the list buffers.
 */
static void
ndis_txeof(adapter, packet, status)
	ndis_handle		adapter;
	ndis_packet		*packet;
	ndis_status		status;

{
	struct ndis_softc	*sc;
	ndis_miniport_block	*block;
	struct ifnet		*ifp;
	int			idx;
	struct mbuf		*m;

	block = (ndis_miniport_block *)adapter;
	sc = device_get_softc(block->nmb_physdeviceobj->do_devext);
	ifp = sc->ifp;

	m = packet->np_m0;
	idx = packet->np_txidx;
	if (sc->ndis_sc)
		bus_dmamap_unload(sc->ndis_ttag, sc->ndis_tmaps[idx]);

	ndis_free_packet(packet);
	m_freem(m);

	NDIS_LOCK(sc);
	sc->ndis_txarray[idx] = NULL;
	sc->ndis_txpending++;

	if (status == NDIS_STATUS_SUCCESS)
		ifp->if_opackets++;
	else
		ifp->if_oerrors++;

	ifp->if_timer = 0;
	ifp->if_drv_flags &= ~IFF_DRV_OACTIVE;

	NDIS_UNLOCK(sc);

	IoQueueWorkItem(sc->ndis_startitem,
	    (io_workitem_func)ndis_starttask_wrap,
	    WORKQUEUE_CRITICAL, ifp);

	return;
}

static void
ndis_linksts(adapter, status, sbuf, slen)
	ndis_handle		adapter;
	ndis_status		status;
	void			*sbuf;
	uint32_t		slen;
{
	ndis_miniport_block	*block;
	struct ndis_softc	*sc;

	block = adapter;
	sc = device_get_softc(block->nmb_physdeviceobj->do_devext);
	sc->ndis_sts = status;

	/* Event list is all full up, drop this one. */

	NDIS_LOCK(sc);
	if (sc->ndis_evt[sc->ndis_evtpidx].ne_sts) {
		NDIS_UNLOCK(sc);
		return;
	}

	/* Cache the event. */

	if (slen) {
		sc->ndis_evt[sc->ndis_evtpidx].ne_buf = malloc(slen,
		    M_TEMP, M_NOWAIT);
		if (sc->ndis_evt[sc->ndis_evtpidx].ne_buf == NULL) {
			NDIS_UNLOCK(sc);
			return;
		}
		bcopy((char *)sbuf,
		    sc->ndis_evt[sc->ndis_evtpidx].ne_buf, slen);
	}
	sc->ndis_evt[sc->ndis_evtpidx].ne_sts = status;
	sc->ndis_evt[sc->ndis_evtpidx].ne_len = slen;
	NDIS_EVTINC(sc->ndis_evtpidx);
	NDIS_UNLOCK(sc);

	return;
}

static void
ndis_linksts_done(adapter)
	ndis_handle		adapter;
{
	ndis_miniport_block	*block;
	struct ndis_softc	*sc;
	struct ifnet		*ifp;

	block = adapter;
	sc = device_get_softc(block->nmb_physdeviceobj->do_devext);
	ifp = sc->ifp;

	if (!NDIS_INITIALIZED(sc))
		return;

	switch (sc->ndis_sts) {
	case NDIS_STATUS_MEDIA_CONNECT:
		IoQueueWorkItem(sc->ndis_tickitem, 
		    (io_workitem_func)ndis_ticktask_wrap,
		    WORKQUEUE_CRITICAL, sc);
		IoQueueWorkItem(sc->ndis_startitem,
		    (io_workitem_func)ndis_starttask_wrap,
		    WORKQUEUE_CRITICAL, ifp);
		break;
	case NDIS_STATUS_MEDIA_DISCONNECT:
		if (sc->ndis_link)
			IoQueueWorkItem(sc->ndis_tickitem,
		    	    (io_workitem_func)ndis_ticktask_wrap,
			    WORKQUEUE_CRITICAL, sc);
		break;
	default:
		break;
	}

	/* Notify possible listners of interface change. */

	rt_ifmsg(ifp);

	return;
}

static void
ndis_tick(xsc)
	void			*xsc;
{
	struct ndis_softc	*sc;

	mtx_unlock(&Giant);

	sc = xsc;

	IoQueueWorkItem(sc->ndis_tickitem,
	    (io_workitem_func)ndis_ticktask_wrap,
	    WORKQUEUE_CRITICAL, sc);
	sc->ndis_stat_ch = timeout(ndis_tick, sc, hz *
	    sc->ndis_block->nmb_checkforhangsecs);

	mtx_lock(&Giant);

	return;
}

static void
ndis_ticktask(d, xsc)
	device_object		*d;
	void			*xsc;
{
	struct ndis_softc	*sc;
	struct ieee80211com	*ic;
	ndis_checkforhang_handler hangfunc;
	uint8_t			rval;

	sc = xsc;
	ic = &sc->ic;

	NDIS_LOCK(sc);
	if (!NDIS_INITIALIZED(sc)) {
		NDIS_UNLOCK(sc);
		return;
	}
	NDIS_UNLOCK(sc);

	hangfunc = sc->ndis_chars->nmc_checkhang_func;

	if (hangfunc != NULL) {
		rval = MSCALL1(hangfunc,
		    sc->ndis_block->nmb_miniportadapterctx);
		if (rval == TRUE) {
			ndis_reset_nic(sc);
			return;
		}
	}

	NDIS_LOCK(sc);
	if (sc->ndis_link == 0 &&
	    sc->ndis_sts == NDIS_STATUS_MEDIA_CONNECT) {
		sc->ndis_link = 1;
		NDIS_UNLOCK(sc);
		if (sc->ndis_80211) {
			ndis_getstate_80211(sc);
			ieee80211_new_state(ic, IEEE80211_S_RUN, -1);
		}
		NDIS_LOCK(sc);
#ifdef LINK_STATE_UP
#if __FreeBSD_version > 600006
		if_link_state_change(sc->ifp, LINK_STATE_UP);
#else
		sc->ifp->if_link_state = LINK_STATE_UP;
		rt_ifmsg(sc->ifp);
#endif
#else
		device_printf(sc->ndis_dev, "link state changed to UP\n");
#endif /* LINK_STATE_UP */
	}

	if (sc->ndis_link == 1 &&
	    sc->ndis_sts == NDIS_STATUS_MEDIA_DISCONNECT) {
		sc->ndis_link = 0;
		if (sc->ndis_80211)
			ieee80211_new_state(ic, IEEE80211_S_SCAN, 0);
#ifdef LINK_STATE_DOWN
#if __FreeBSD_version > 600006
		if_link_state_change(sc->ifp, LINK_STATE_DOWN);
#else
		sc->ifp->if_link_state = LINK_STATE_DOWN;
		rt_ifmsg(sc->ifp);
#endif
#else
		device_printf(sc->ndis_dev, "link state changed to DOWN\n");
#endif /* LINK_STATE_DOWN */
	}

	NDIS_UNLOCK(sc);

	return;
}

static void
ndis_map_sclist(arg, segs, nseg, mapsize, error)
	void			*arg;
	bus_dma_segment_t	*segs;
	int			nseg;
	bus_size_t		mapsize;
	int			error;

{
	struct ndis_sc_list	*sclist;
	int			i;

	if (error || arg == NULL)
		return;

	sclist = arg;

	sclist->nsl_frags = nseg;

	for (i = 0; i < nseg; i++) {
		sclist->nsl_elements[i].nse_addr.np_quad = segs[i].ds_addr;
		sclist->nsl_elements[i].nse_len = segs[i].ds_len;
	}

	return;
}

static void
ndis_starttask(d, arg)
	device_object		*d;
	void			*arg;
{
	struct ifnet		*ifp;

	ifp = arg;

#if __FreeBSD_version < 502114
	if (ifp->if_snd.ifq_head != NULL)
#else
	if (!IFQ_DRV_IS_EMPTY(&ifp->if_snd))
#endif
		ndis_start(ifp);
	return;
}

/*
 * Main transmit routine. To make NDIS drivers happy, we need to
 * transform mbuf chains into NDIS packets and feed them to the
 * send packet routines. Most drivers allow you to send several
 * packets at once (up to the maxpkts limit). Unfortunately, rather
 * that accepting them in the form of a linked list, they expect
 * a contiguous array of pointers to packets.
 *
 * For those drivers which use the NDIS scatter/gather DMA mechanism,
 * we need to perform busdma work here. Those that use map registers
 * will do the mapping themselves on a buffer by buffer basis.
 */

static void
ndis_start(ifp)
	struct ifnet		*ifp;
{
	struct ndis_softc	*sc;
	struct mbuf		*m = NULL;
	ndis_packet		**p0 = NULL, *p = NULL;
	ndis_tcpip_csum		*csum;
	int			pcnt = 0, status;

	sc = ifp->if_softc;

	NDIS_LOCK(sc);

	if (!sc->ndis_link || ifp->if_drv_flags & IFF_DRV_OACTIVE) {
		NDIS_UNLOCK(sc);
		return;
	}

	p0 = &sc->ndis_txarray[sc->ndis_txidx];

	while(sc->ndis_txpending) {
#if __FreeBSD_version < 502114
		IF_DEQUEUE(&ifp->if_snd, m);
#else
		IFQ_DRV_DEQUEUE(&ifp->if_snd, m);
#endif
		if (m == NULL)
			break;

		NdisAllocatePacket(&status,
		    &sc->ndis_txarray[sc->ndis_txidx], sc->ndis_txpool);

		if (status != NDIS_STATUS_SUCCESS)
			break;

		if (ndis_mtop(m, &sc->ndis_txarray[sc->ndis_txidx])) {
#if __FreeBSD_version >= 502114
			IFQ_DRV_PREPEND(&ifp->if_snd, m);
#endif
			NDIS_UNLOCK(sc);
#if __FreeBSD_version < 502114
			IF_PREPEND(&ifp->if_snd, m);
#endif
			return;
		}

		/*
		 * Save pointer to original mbuf
		 * so we can free it later.
		 */

		p = sc->ndis_txarray[sc->ndis_txidx];
		p->np_txidx = sc->ndis_txidx;
		p->np_m0 = m;
		p->np_oob.npo_status = NDIS_STATUS_PENDING;

		/*
		 * Do scatter/gather processing, if driver requested it.
		 */
		if (sc->ndis_sc) {
			bus_dmamap_load_mbuf(sc->ndis_ttag,
			    sc->ndis_tmaps[sc->ndis_txidx], m,
			    ndis_map_sclist, &p->np_sclist, BUS_DMA_NOWAIT);
			bus_dmamap_sync(sc->ndis_ttag,
			    sc->ndis_tmaps[sc->ndis_txidx],
			    BUS_DMASYNC_PREREAD);
			p->np_ext.npe_info[ndis_sclist_info] = &p->np_sclist;
		}

		/* Handle checksum offload. */

		if (ifp->if_capenable & IFCAP_TXCSUM &&
		    m->m_pkthdr.csum_flags) {
			csum = (ndis_tcpip_csum *)
				&p->np_ext.npe_info[ndis_tcpipcsum_info];
			csum->u.ntc_txflags = NDIS_TXCSUM_DO_IPV4;
			if (m->m_pkthdr.csum_flags & CSUM_IP)
				csum->u.ntc_txflags |= NDIS_TXCSUM_DO_IP;
			if (m->m_pkthdr.csum_flags & CSUM_TCP)
				csum->u.ntc_txflags |= NDIS_TXCSUM_DO_TCP;
			if (m->m_pkthdr.csum_flags & CSUM_UDP)
				csum->u.ntc_txflags |= NDIS_TXCSUM_DO_UDP;
			p->np_private.npp_flags = NDIS_PROTOCOL_ID_TCP_IP;
		}

		NDIS_INC(sc);
		sc->ndis_txpending--;

		pcnt++;

		/*
		 * If there's a BPF listener, bounce a copy of this frame
		 * to him.
		 */

		BPF_MTAP(ifp, m);

		/*
		 * The array that p0 points to must appear contiguous,
		 * so we must not wrap past the end of sc->ndis_txarray[].
		 * If it looks like we're about to wrap, break out here
		 * so the this batch of packets can be transmitted, then
		 * wait for txeof to ask us to send the rest.
		 */

		if (sc->ndis_txidx == 0)
			break;
	}

	if (pcnt == 0) {
		NDIS_UNLOCK(sc);
		return;
	}

	if (sc->ndis_txpending == 0)
		ifp->if_drv_flags |= IFF_DRV_OACTIVE;

	/*
	 * Set a timeout in case the chip goes out to lunch.
	 */
	ifp->if_timer = 5;

	NDIS_UNLOCK(sc);

	/*
	 * According to NDIS documentation, if a driver exports
	 * a MiniportSendPackets() routine, we prefer that over
	 * a MiniportSend() routine (which sends just a single
	 * packet).
	 */

	if (sc->ndis_chars->nmc_sendmulti_func != NULL)
		ndis_send_packets(sc, p0, pcnt);
	else
		ndis_send_packet(sc, p);

	return;
}

static void
ndis_init(xsc)
	void			*xsc;
{
	struct ndis_softc	*sc = xsc;
	struct ieee80211com	*ic = (void *)&sc->ic;
	struct ifnet		*ifp = sc->ifp;
	int			i, len, error;

	/*
	 * Avoid reintializing the link unnecessarily.
	 * This should be dealt with in a better way by
	 * fixing the upper layer modules so they don't
	 * call ifp->if_init() quite as often.
	 */
	if (sc->ndis_link && sc->ndis_skip)
		return;

	/*
	 * Cancel pending I/O and free all RX/TX buffers.
	 */
	ndis_stop(sc);

	if (ndis_init_nic(sc))
		return;

	/* Init our MAC address */

	/* Program the packet filter */

	sc->ndis_filter = NDIS_PACKET_TYPE_DIRECTED;

	if (ifp->if_flags & IFF_BROADCAST)
		sc->ndis_filter |= NDIS_PACKET_TYPE_BROADCAST;

	if (ifp->if_flags & IFF_PROMISC)
		sc->ndis_filter |= NDIS_PACKET_TYPE_PROMISCUOUS;

	len = sizeof(sc->ndis_filter);

	error = ndis_set_info(sc, OID_GEN_CURRENT_PACKET_FILTER,
	    &sc->ndis_filter, &len);

	if (error)
		device_printf (sc->ndis_dev, "set filter failed: %d\n", error);

	/*
	 * Set lookahead.
 	 */

	i = ifp->if_mtu;
	len = sizeof(i);
	ndis_set_info(sc, OID_GEN_CURRENT_LOOKAHEAD, &i, &len);

	/*
	 * Program the multicast filter, if necessary.
	 */
	ndis_setmulti(sc);

	/* Setup task offload. */
	ndis_set_offload(sc);

	if (sc->ndis_80211)
		ndis_setstate_80211(sc);

	NDIS_LOCK(sc);

	sc->ndis_txidx = 0;
	sc->ndis_txpending = sc->ndis_maxpkts;
	sc->ndis_link = 0;

#ifdef LINK_STATE_UNKNOWN
#if __FreeBSD_version > 600006
	if_link_state_change(sc->ifp, LINK_STATE_UNKNOWN);
#else
	sc->ifp->if_link_state = LINK_STATE_DOWN;
	rt_ifmsg(sc->ifp);
#endif
#endif /* LINK_STATE_UNKNOWN */

	if (ic->ic_opmode != IEEE80211_M_MONITOR) {
		/*
		 * NB: When restarting the adapter clock the state
		 * machine regardless of the roaming mode; otherwise
		 * we need to notify user apps so they can manually
		 * get us going again.
		 */
		if (ic->ic_roaming != IEEE80211_ROAMING_MANUAL)
			ieee80211_new_state(ic, IEEE80211_S_SCAN, 0);
	} else
		ieee80211_new_state(ic, IEEE80211_S_RUN, -1);

	ifp->if_drv_flags |= IFF_DRV_RUNNING;
	ifp->if_drv_flags &= ~IFF_DRV_OACTIVE;

	NDIS_UNLOCK(sc);

	/*
	 * Some drivers don't set this value. The NDIS spec says
	 * the default checkforhang timeout is "approximately 2
	 * seconds." We use 3 seconds, because it seems for some
	 * drivers, exactly 2 seconds is too fast.
	 */

	if (sc->ndis_block->nmb_checkforhangsecs == 0)
		sc->ndis_block->nmb_checkforhangsecs = 3;

	sc->ndis_stat_ch = timeout(ndis_tick, sc,
	    hz * sc->ndis_block->nmb_checkforhangsecs);

	return;
}

/*
 * Set media options.
 */
static int
ndis_ifmedia_upd(ifp)
	struct ifnet		*ifp;
{
	struct ndis_softc		*sc;

	sc = ifp->if_softc;

	if (NDIS_INITIALIZED(sc))
		ndis_init(sc);

	return(0);
}

/*
 * Report current media status.
 */
static void
ndis_ifmedia_sts(ifp, ifmr)
	struct ifnet		*ifp;
	struct ifmediareq	*ifmr;
{
	struct ndis_softc	*sc;
	uint32_t		media_info;
	ndis_media_state	linkstate;
	int			error, len;

	ifmr->ifm_status = IFM_AVALID;
	ifmr->ifm_active = IFM_ETHER;
	sc = ifp->if_softc;

	if (!NDIS_INITIALIZED(sc))
		return;

	len = sizeof(linkstate);
	error = ndis_get_info(sc, OID_GEN_MEDIA_CONNECT_STATUS,
	    (void *)&linkstate, &len);

	len = sizeof(media_info);
	error = ndis_get_info(sc, OID_GEN_LINK_SPEED,
	    (void *)&media_info, &len);

	if (linkstate == nmc_connected)
		ifmr->ifm_status |= IFM_ACTIVE;

	switch(media_info) {
	case 100000:
		ifmr->ifm_active |= IFM_10_T;
		break;
	case 1000000:
		ifmr->ifm_active |= IFM_100_TX;
		break;
	case 10000000:
		ifmr->ifm_active |= IFM_1000_T;
		break;
	default:
		device_printf(sc->ndis_dev, "unknown speed: %d\n", media_info);
		break;
	}

	return;
}

#ifdef IEEE80211_F_WPA
static int
ndis_set_cipher(sc, cipher)
	struct ndis_softc	*sc;
	int			cipher;
{
	struct ieee80211com	*ic;
	int			rval = 0, len;
	uint32_t		arg, save;

	ic = &sc->ic;

	len = sizeof(arg);

	if (cipher == WPA_CSE_WEP40 || WPA_CSE_WEP104) {
		if (!(ic->ic_caps & IEEE80211_C_WEP))
			return(ENOTSUP);
		arg = NDIS_80211_WEPSTAT_ENC1ENABLED;
	}

	if (cipher == WPA_CSE_TKIP) {
		if (!(ic->ic_caps & IEEE80211_C_TKIP))
			return(ENOTSUP);
		arg = NDIS_80211_WEPSTAT_ENC2ENABLED;
	}

	if (cipher == WPA_CSE_CCMP) {
		if (!(ic->ic_caps & IEEE80211_C_AES_CCM))
			return(ENOTSUP);
		arg = NDIS_80211_WEPSTAT_ENC3ENABLED;
	}

	DPRINTF(("Setting cipher to %d\n", arg));
	save = arg;
	rval = ndis_set_info(sc, OID_802_11_ENCRYPTION_STATUS, &arg, &len);

	if (rval)
		return(rval);

	/* Check that the cipher was set correctly. */

	len = sizeof(save);
	rval = ndis_get_info(sc, OID_802_11_ENCRYPTION_STATUS, &arg, &len);

	if (rval != 0 || arg != save)
		return(ENODEV);

	return(0);
}

/*
 * WPA is hairy to set up. Do the work in a separate routine
 * so we don't clutter the setstate function too much.
 * Important yet undocumented fact: first we have to set the
 * authentication mode, _then_ we enable the ciphers. If one
 * of the WPA authentication modes isn't enabled, the driver
 * might not permit the TKIP or AES ciphers to be selected.
 */

static int
ndis_set_wpa(sc)
	struct ndis_softc	*sc;
{
	struct ieee80211com	*ic;
	struct ieee80211_ie_wpa	*w;
	struct ndis_ie		*n;
	char			*pos;
	uint32_t		arg;
	int			i;

	ic = &sc->ic;

	/*
	 * Apparently, the only way for us to know what ciphers
	 * and key management/authentication mode to use is for
	 * us to inspect the optional information element (IE)
	 * stored in the 802.11 state machine. This IE should be
	 * supplied by the WPA supplicant.
	 */

	w = (struct ieee80211_ie_wpa *)ic->ic_opt_ie;

	/* Check for the right kind of IE. */
	if (w->wpa_id != IEEE80211_ELEMID_VENDOR) {
		DPRINTF(("Incorrect IE type %d\n", w->wpa_id));
		return(EINVAL);
	}

	/* Skip over the ucast cipher OIDs. */
	pos = (char *)&w->wpa_uciphers[0];
	pos += w->wpa_uciphercnt * sizeof(struct ndis_ie);

	/* Skip over the authmode count. */
	pos += sizeof(u_int16_t);

	/*
	 * Check for the authentication modes. I'm
	 * pretty sure there's only supposed to be one.
	 */

	n = (struct ndis_ie *)pos;
	if (n->ni_val == WPA_ASE_NONE)
		arg = NDIS_80211_AUTHMODE_WPANONE;

	if (n->ni_val == WPA_ASE_8021X_UNSPEC)
		arg = NDIS_80211_AUTHMODE_WPA;

	if (n->ni_val == WPA_ASE_8021X_PSK)
		arg = NDIS_80211_AUTHMODE_WPAPSK;

	DPRINTF(("Setting WPA auth mode to %d\n", arg));
	i = sizeof(arg);
	if (ndis_set_info(sc, OID_802_11_AUTHENTICATION_MODE, &arg, &i))
		return(ENOTSUP);
	i = sizeof(arg);
	ndis_get_info(sc, OID_802_11_AUTHENTICATION_MODE, &arg, &i);

	/* Now configure the desired ciphers. */

	/* First, set up the multicast group cipher. */
	n = (struct ndis_ie *)&w->wpa_mcipher[0];

	if (ndis_set_cipher(sc, n->ni_val))
		return(ENOTSUP);

	/* Now start looking around for the unicast ciphers. */
	pos = (char *)&w->wpa_uciphers[0];
	n = (struct ndis_ie *)pos;

	for (i = 0; i < w->wpa_uciphercnt; i++) {
		if (ndis_set_cipher(sc, n->ni_val))
			return(ENOTSUP);
		n++;
	}

	return(0);
}
#endif

static void
ndis_setstate_80211(sc)
	struct ndis_softc	*sc;
{
	struct ieee80211com	*ic;
	struct ieee80211_node	*ni;
	ndis_80211_ssid		ssid;
	ndis_80211_macaddr	bssid;
	ndis_80211_config	config;
	ndis_80211_wep		wep;
	int			i, rval = 0, len;
	uint32_t		arg;
	struct ifnet		*ifp;

	ic = &sc->ic;
	ifp = sc->ifp;
	ni = ic->ic_bss;

	if (!NDIS_INITIALIZED(sc)) {
		DPRINTF(("%s: NDIS not initialized\n", __func__));
		return;
	}

	/* Disassociate and turn off radio. */

	len = sizeof(arg);
	arg = 1;
	ndis_set_info(sc, OID_802_11_DISASSOCIATE, &arg, &len);

	/* Set network infrastructure mode. */

	len = sizeof(arg);
	if (ic->ic_opmode == IEEE80211_M_IBSS)
		arg = NDIS_80211_NET_INFRA_IBSS;
	else
		arg = NDIS_80211_NET_INFRA_BSS;

	rval = ndis_set_info(sc, OID_802_11_INFRASTRUCTURE_MODE, &arg, &len);

	if (rval)
		device_printf (sc->ndis_dev, "set infra failed: %d\n", rval);

	/* Set RTS threshold */

	len = sizeof(arg);
	arg = ic->ic_rtsthreshold;
	ndis_set_info(sc, OID_802_11_RTS_THRESHOLD, &arg, &len);

	/* Set fragmentation threshold */

	len = sizeof(arg);
	arg = ic->ic_fragthreshold;
	ndis_set_info(sc, OID_802_11_FRAGMENTATION_THRESHOLD, &arg, &len);

	/* Set power management */

	len = sizeof(arg);
	if (ic->ic_flags & IEEE80211_F_PMGTON)
		arg = NDIS_80211_POWERMODE_FAST_PSP;
	else
		arg = NDIS_80211_POWERMODE_CAM;
	ndis_set_info(sc, OID_802_11_POWER_MODE, &arg, &len);

	/*
	 * Default encryption mode to off, authentication
	 * to open and privacy to 'accept everything.'
	 */

	len = sizeof(arg);
	arg = NDIS_80211_WEPSTAT_DISABLED;
	ndis_set_info(sc, OID_802_11_ENCRYPTION_STATUS, &arg, &len);

	len = sizeof(arg);
	arg = NDIS_80211_AUTHMODE_OPEN;
	ndis_set_info(sc, OID_802_11_AUTHENTICATION_MODE, &arg, &len);

	/*
	 * Note that OID_80211_PRIVACY_FILTER is optional:
	 * not all drivers implement it.
	 */
	len = sizeof(arg);
	arg = NDIS_80211_PRIVFILT_8021XWEP;
	ndis_set_info(sc, OID_802_11_PRIVACY_FILTER, &arg, &len);

	/* Set WEP */

#if __FreeBSD_version < 600007
	if (ic->ic_flags & IEEE80211_F_WEPON) {
#else
	if (ic->ic_flags & IEEE80211_F_PRIVACY &&
	    !(ic->ic_flags & IEEE80211_F_WPA)) {
#endif
		int			keys_set = 0;

#if __FreeBSD_version >= 600007
		if (ic->ic_bss->ni_authmode == IEEE80211_AUTH_SHARED) {
			len = sizeof(arg);
			arg = NDIS_80211_AUTHMODE_SHARED;
			DPRINTF(("Setting shared auth\n"));
			ndis_set_info(sc, OID_802_11_AUTHENTICATION_MODE,
			    &arg, &len);
		}
#endif
		for (i = 0; i < IEEE80211_WEP_NKID; i++) {
			if (ic->ic_nw_keys[i].wk_keylen) {
#if __FreeBSD_version >= 600007
				if (ic->ic_nw_keys[i].wk_cipher->ic_cipher !=
				    IEEE80211_CIPHER_WEP)
					continue;
#endif
				bzero((char *)&wep, sizeof(wep));
				wep.nw_keylen = ic->ic_nw_keys[i].wk_keylen;

				/*
				 * 5, 13 and 16 are the only valid
				 * only valid key lengths. Anything
				 * in between will be zero padded out to
				 * the next highest boundary.
				 */
				if (ic->ic_nw_keys[i].wk_keylen < 5)
					wep.nw_keylen = 5;
				else if (ic->ic_nw_keys[i].wk_keylen > 5 &&
				     ic->ic_nw_keys[i].wk_keylen < 13)
					wep.nw_keylen = 13;
				else if (ic->ic_nw_keys[i].wk_keylen > 13 &&
				     ic->ic_nw_keys[i].wk_keylen < 16)
					wep.nw_keylen = 16;

				wep.nw_keyidx = i;
				wep.nw_length = (sizeof(uint32_t) * 3)
				    + wep.nw_keylen;
				if (i == ic->ic_def_txkey)
					wep.nw_keyidx |= NDIS_80211_WEPKEY_TX;
				bcopy(ic->ic_nw_keys[i].wk_key,
				    wep.nw_keydata, wep.nw_length);
				len = sizeof(wep);
				DPRINTF(("Setting WEP key %d\n", i));
				rval = ndis_set_info(sc,
				    OID_802_11_ADD_WEP, &wep, &len);
				if (rval)
					device_printf(sc->ndis_dev,
					    "set wepkey failed: %d\n", rval);
				keys_set++;
			}
		}
		if (keys_set) {
			DPRINTF(("Setting WEP on\n"));
			arg = NDIS_80211_WEPSTAT_ENABLED;
			len = sizeof(arg);
			rval = ndis_set_info(sc,
			    OID_802_11_WEP_STATUS, &arg, &len);
			if (rval)
				device_printf(sc->ndis_dev,
				    "enable WEP failed: %d\n", rval);
#ifdef IEEE80211_F_DROPUNENC
			if (ic->ic_flags & IEEE80211_F_DROPUNENC)
				arg = NDIS_80211_PRIVFILT_8021XWEP;
			else
				arg = NDIS_80211_PRIVFILT_ACCEPTALL;

			len = sizeof(arg);
			ndis_set_info(sc,
			    OID_802_11_PRIVACY_FILTER, &arg, &len);
#endif
		}
	} 

#ifdef IEEE80211_F_WPA
	/* Set up WPA. */

	if (ic->ic_flags & IEEE80211_F_WPA1 && ic->ic_opt_ie_len &&
	    ic->ic_caps & IEEE80211_C_WPA)
		if (ndis_set_wpa(sc))
			device_printf(sc->ndis_dev, "WPA setup failed\n");
#endif

#ifdef notyet
	/* Set network type. */

	arg = 0;

	switch (ic->ic_curmode) {
	case IEEE80211_MODE_11A:
		arg = NDIS_80211_NETTYPE_11OFDM5;
		break;
	case IEEE80211_MODE_11B:
		arg = NDIS_80211_NETTYPE_11DS;
		break;
	case IEEE80211_MODE_11G:
		arg = NDIS_80211_NETTYPE_11OFDM24;
		break;
	default:
		device_printf(sc->ndis_dev, "unknown mode: %d\n",
		    ic->ic_curmode);
	}

	if (arg) {
		DPRINTF(("Setting network type to %d\n", arg));
		len = sizeof(arg);
		rval = ndis_set_info(sc, OID_802_11_NETWORK_TYPE_IN_USE,
		    &arg, &len);
		if (rval)
			device_printf (sc->ndis_dev,
			    "set nettype failed: %d\n", rval);
	}
#endif

	len = sizeof(config);
	bzero((char *)&config, len);
	config.nc_length = len;
	config.nc_fhconfig.ncf_length = sizeof(ndis_80211_config_fh);
	rval = ndis_get_info(sc, OID_802_11_CONFIGURATION, &config, &len); 

	/*
	 * Some drivers expect us to initialize these values, so
	 * provide some defaults.
	 */

	if (config.nc_beaconperiod == 0)
		config.nc_beaconperiod = 100;
	if (config.nc_atimwin == 0)
		config.nc_atimwin = 100;
	if (config.nc_fhconfig.ncf_dwelltime == 0)
		config.nc_fhconfig.ncf_dwelltime = 200;
	if (rval == 0 && ic->ic_bsschan != IEEE80211_CHAN_ANYC) { 
		int chan, chanflag;

		chan = ieee80211_chan2ieee(ic, ic->ic_bsschan);
		chanflag = config.nc_dsconfig > 2500000 ? IEEE80211_CHAN_2GHZ :
		    IEEE80211_CHAN_5GHZ;
		if (chan != ieee80211_mhz2ieee(config.nc_dsconfig / 1000, 0)) {
			config.nc_dsconfig =
				ic->ic_bsschan->ic_freq * 1000;
			ic->ic_bss->ni_chan = ic->ic_bsschan;
			len = sizeof(config);
			config.nc_length = len;
			config.nc_fhconfig.ncf_length =
			    sizeof(ndis_80211_config_fh);
			DPRINTF(("Setting channel to %ukHz\n", config.nc_dsconfig));
			rval = ndis_set_info(sc, OID_802_11_CONFIGURATION,
			    &config, &len);
			if (rval)
				device_printf(sc->ndis_dev, "couldn't change "
				    "DS config to %ukHz: %d\n",
				    config.nc_dsconfig, rval);
		}
	} else if (rval)
		device_printf(sc->ndis_dev, "couldn't retrieve "
		    "channel info: %d\n", rval);

	/*
	 * If the user selected a specific BSSID, try
	 * to use that one. This is useful in the case where
	 * there are several APs in range with the same network
	 * name. To delete the BSSID, we use the broadcast
	 * address as the BSSID.
	 * Note that some drivers seem to allow setting a BSSID
	 * in ad-hoc mode, which has the effect of forcing the
	 * NIC to create an ad-hoc cell with a specific BSSID,
	 * instead of a randomly chosen one. However, the net80211
	 * code makes the assumtion that the BSSID setting is invalid
	 * when you're in ad-hoc mode, so we don't allow that here.
	 */

	len = IEEE80211_ADDR_LEN;
	if (ic->ic_flags & IEEE80211_F_DESBSSID &&
	    ic->ic_opmode != IEEE80211_M_IBSS)
		bcopy(ni->ni_bssid, bssid, len);
	else
		bcopy(ifp->if_broadcastaddr, bssid, len);

	DPRINTF(("Setting BSSID to %6D\n", &bssid, ":"));
	rval = ndis_set_info(sc, OID_802_11_BSSID, &bssid, &len);
	if (rval)
		device_printf(sc->ndis_dev,
		    "setting BSSID failed: %d\n", rval);

	/* Set SSID -- always do this last. */

#ifdef NDIS_DEBUG
	printf("Setting ESSID to ");
	ieee80211_print_essid(ni->ni_essid, ni->ni_esslen);
	printf("\n");
#endif

	len = sizeof(ssid);
	bzero((char *)&ssid, len);
	ssid.ns_ssidlen = ni->ni_esslen;
	if (ssid.ns_ssidlen == 0) {
		ssid.ns_ssidlen = 1;
	} else
		bcopy(ni->ni_essid, ssid.ns_ssid, ssid.ns_ssidlen);

	rval = ndis_set_info(sc, OID_802_11_SSID, &ssid, &len);

	if (rval)
		device_printf (sc->ndis_dev, "set ssid failed: %d\n", rval);

	if (ic->ic_state == IEEE80211_S_AUTH)
		ieee80211_new_state(ic, IEEE80211_S_ASSOC, 0);

	return;
}

static void
ndis_media_status(struct ifnet *ifp, struct ifmediareq *imr)
{
        struct ieee80211com *ic = &((struct ndis_softc *)ifp->if_softc)->ic;
        struct ieee80211_node *ni = NULL;

        imr->ifm_status = IFM_AVALID;
        imr->ifm_active = IFM_IEEE80211;
        if (ic->ic_state == IEEE80211_S_RUN)
                imr->ifm_status |= IFM_ACTIVE;
        imr->ifm_active |= IFM_AUTO;
        switch (ic->ic_opmode) {
        case IEEE80211_M_STA:
                ni = ic->ic_bss;
                /* calculate rate subtype */
                imr->ifm_active |= ieee80211_rate2media(ic,
                        ni->ni_rates.rs_rates[ni->ni_txrate], ic->ic_curmode);
                break;
        case IEEE80211_M_IBSS:
                ni = ic->ic_bss;
                /* calculate rate subtype */
                imr->ifm_active |= ieee80211_rate2media(ic,
                        ni->ni_rates.rs_rates[ni->ni_txrate], ic->ic_curmode);
                imr->ifm_active |= IFM_IEEE80211_ADHOC;
                break;
        case IEEE80211_M_AHDEMO:
                /* should not come here */
                break;
        case IEEE80211_M_HOSTAP:
                imr->ifm_active |= IFM_IEEE80211_HOSTAP;
                break;
        case IEEE80211_M_MONITOR:
                imr->ifm_active |= IFM_IEEE80211_MONITOR;
                break;
	case IEEE80211_M_WDS:
		printf("WARNING: WDS operation mode not supported by NDIS\n");
		break;
        }
        switch (ic->ic_curmode) {
        case IEEE80211_MODE_11A:
                imr->ifm_active |= IFM_MAKEMODE(IFM_IEEE80211_11A);
                break;
        case IEEE80211_MODE_11B:
                imr->ifm_active |= IFM_MAKEMODE(IFM_IEEE80211_11B);
                break;
        case IEEE80211_MODE_11G:
                imr->ifm_active |= IFM_MAKEMODE(IFM_IEEE80211_11G);
                break;
#ifdef IEEE80211_MODE_TURBO_A
        case IEEE80211_MODE_TURBO_A:
                imr->ifm_active |= IFM_MAKEMODE(IFM_IEEE80211_11A)
                                |  IFM_IEEE80211_TURBO;
#endif
                break;
        }
}

static int
ndis_get_assoc(sc, assoc)
	struct ndis_softc	*sc;
	ndis_wlan_bssid_ex	**assoc;
{
	ndis_80211_bssid_list_ex	*bl;
	ndis_wlan_bssid_ex	*bs;
	ndis_80211_macaddr	bssid;
	int			i, len, error;

	if (!sc->ndis_link)
		return(ENOENT);

	len = sizeof(bssid);
	error = ndis_get_info(sc, OID_802_11_BSSID, &bssid, &len);
	if (error) {
		device_printf(sc->ndis_dev, "failed to get bssid\n");
		return(ENOENT);
	}

	len = 4;
	error = ndis_get_info(sc, OID_802_11_BSSID_LIST, NULL, &len);
	if (error != ENOSPC)
		len = 65536;

	bl = malloc(len, M_TEMP, M_NOWAIT|M_ZERO);
	error = ndis_get_info(sc, OID_802_11_BSSID_LIST, bl, &len);
	if (error) {
		free(bl, M_TEMP);
		device_printf(sc->ndis_dev, "bssid_list failed\n");
		return (error);
	}

	bs = (ndis_wlan_bssid_ex *)&bl->nblx_bssid[0];
	for (i = 0; i < bl->nblx_items; i++) {
		if (bcmp(bs->nwbx_macaddr, bssid, sizeof(bssid)) == 0) {
			*assoc = malloc(bs->nwbx_len, M_TEMP, M_NOWAIT);
			if (*assoc == NULL) {
				free(bl, M_TEMP);
				return(ENOMEM);
			}
			bcopy((char *)bs, (char *)*assoc, bs->nwbx_len);
			free(bl, M_TEMP);
			return(0);
		}	
		bs = (ndis_wlan_bssid_ex *)((char *)bs + bs->nwbx_len);
	}

	free(bl, M_TEMP);
	return(ENOENT);
}

static void
ndis_getstate_80211(sc)
	struct ndis_softc	*sc;
{
	struct ieee80211com	*ic;
	ndis_wlan_bssid_ex	*bs;
	int			rval, len, i = 0;
	int			chanflag;
	uint32_t		arg;
	struct ifnet		*ifp;

	ic = &sc->ic;
	ifp = sc->ifp;

	if (!NDIS_INITIALIZED(sc))
		return;

	/*
	 * If we're associated, retrieve info on the current bssid.
	 */
	if ((rval = ndis_get_assoc(sc, &bs)) == 0) {
		switch(bs->nwbx_nettype) {
		case NDIS_80211_NETTYPE_11FH:
		case NDIS_80211_NETTYPE_11DS:
			ic->ic_curmode = IEEE80211_MODE_11B;
			chanflag = IEEE80211_CHAN_B;
			break;
		case NDIS_80211_NETTYPE_11OFDM5:
			ic->ic_curmode = IEEE80211_MODE_11A;
			chanflag = IEEE80211_CHAN_A;
			break;
		case NDIS_80211_NETTYPE_11OFDM24:
			ic->ic_curmode = IEEE80211_MODE_11G;
			chanflag = IEEE80211_CHAN_G;
			break;
		default:
			device_printf(sc->ndis_dev,
			    "unknown nettype %d\n", arg);
			chanflag = 0;
			break;
		}
		IEEE80211_ADDR_COPY(ic->ic_bss->ni_bssid, bs->nwbx_macaddr);
	} else
		return;

	/* Get SSID from current association info. */

	bcopy(bs->nwbx_ssid.ns_ssid, ic->ic_bss->ni_essid,
	    bs->nwbx_ssid.ns_ssidlen);
	ic->ic_bss->ni_esslen = bs->nwbx_ssid.ns_ssidlen;

	len = sizeof(arg);
	rval = ndis_get_info(sc, OID_GEN_LINK_SPEED, &arg, &len);
	if (rval)
		device_printf (sc->ndis_dev, "get link speed failed: %d\n",
		    rval);

	if (isset(ic->ic_modecaps, IEEE80211_MODE_11B)) {
		ic->ic_bss->ni_rates = ic->ic_sup_rates[IEEE80211_MODE_11B];
		for (i = 0; i < ic->ic_bss->ni_rates.rs_nrates; i++) {
			if ((ic->ic_bss->ni_rates.rs_rates[i] &
			    IEEE80211_RATE_VAL) == arg / 5000)
				break;
		}
	}

	if (i == ic->ic_bss->ni_rates.rs_nrates &&
	    isset(ic->ic_modecaps, IEEE80211_MODE_11G)) {
		ic->ic_bss->ni_rates = ic->ic_sup_rates[IEEE80211_MODE_11G];
		for (i = 0; i < ic->ic_bss->ni_rates.rs_nrates; i++) {
			if ((ic->ic_bss->ni_rates.rs_rates[i] &
			    IEEE80211_RATE_VAL) == arg / 5000)
				break;
		}
	}

	if (i == ic->ic_bss->ni_rates.rs_nrates)
		device_printf(sc->ndis_dev, "no matching rate for: %d\n",
		    arg / 5000);
	else
		ic->ic_bss->ni_txrate = i;

	if (ic->ic_caps & IEEE80211_C_PMGT) {
		len = sizeof(arg);
		rval = ndis_get_info(sc, OID_802_11_POWER_MODE, &arg, &len);

		if (rval)
			device_printf(sc->ndis_dev,
			    "get power mode failed: %d\n", rval);
		if (arg == NDIS_80211_POWERMODE_CAM)
			ic->ic_flags &= ~IEEE80211_F_PMGTON;
		else
			ic->ic_flags |= IEEE80211_F_PMGTON;
	}

	/*
	 * Use the current association information to reflect
	 * what channel we're on.
	 */
	ic->ic_curchan = ieee80211_find_channel(ic,
	    bs->nwbx_config.nc_dsconfig / 1000, chanflag);
	if (ic->ic_curchan == NULL)
		ic->ic_curchan = &ic->ic_channels[0];
	ic->ic_bss->ni_chan = ic->ic_curchan;
	ic->ic_des_chan = ic->ic_curchan;
	ic->ic_bsschan = ic->ic_curchan;

	free(bs, M_TEMP);

	/*
	 * Determine current authentication mode. Note: authmode
	 * reporting isn't supported prior to FreeBSD 6.x.
	 */

#if __FreeBSD_version >= 600007
	len = sizeof(arg);
	rval = ndis_get_info(sc, OID_802_11_AUTHENTICATION_MODE, &arg, &len);
	if (rval)
		device_printf (sc->ndis_dev,
		    "get authmode status failed: %d\n", rval);
	else {
		ic->ic_flags &= ~IEEE80211_F_WPA;
		switch(arg) {
		case NDIS_80211_AUTHMODE_OPEN:
			ic->ic_bss->ni_authmode = IEEE80211_AUTH_OPEN;
			break;
		case NDIS_80211_AUTHMODE_SHARED:
			ic->ic_bss->ni_authmode = IEEE80211_AUTH_SHARED;
			break;
		case NDIS_80211_AUTHMODE_AUTO:
			ic->ic_bss->ni_authmode = IEEE80211_AUTH_AUTO;
			break;
		case NDIS_80211_AUTHMODE_WPA:
		case NDIS_80211_AUTHMODE_WPAPSK:
		case NDIS_80211_AUTHMODE_WPANONE:
			ic->ic_bss->ni_authmode = IEEE80211_AUTH_WPA;
			ic->ic_flags |= IEEE80211_F_WPA1;
			break;
		case NDIS_80211_AUTHMODE_WPA2:
		case NDIS_80211_AUTHMODE_WPA2PSK:
			ic->ic_bss->ni_authmode = IEEE80211_AUTH_WPA;
			ic->ic_flags |= IEEE80211_F_WPA2;
			break;
		default:
			ic->ic_bss->ni_authmode = IEEE80211_AUTH_NONE;
			break;
		}
	}
#endif

	len = sizeof(arg);
	rval = ndis_get_info(sc, OID_802_11_WEP_STATUS, &arg, &len);

	if (rval)
		device_printf (sc->ndis_dev,
		    "get wep status failed: %d\n", rval);

#if __FreeBSD_version < 600007
	if (arg == NDIS_80211_WEPSTAT_ENABLED)
		ic->ic_flags |= IEEE80211_F_WEPON;
	else
		ic->ic_flags &= ~IEEE80211_F_WEPON;
#else
	if (arg == NDIS_80211_WEPSTAT_ENABLED)
		ic->ic_flags |= IEEE80211_F_PRIVACY|IEEE80211_F_DROPUNENC;
	else
		ic->ic_flags &= ~(IEEE80211_F_PRIVACY|IEEE80211_F_DROPUNENC);
		
#endif

	return;
}

static int
ndis_ioctl(ifp, command, data)
	struct ifnet		*ifp;
	u_long			command;
	caddr_t			data;
{
	struct ndis_softc	*sc = ifp->if_softc;
	struct ifreq		*ifr = (struct ifreq *) data;
	struct ndis_oid_data	oid;
	struct ndis_evt		evt;
	void			*oidbuf;
	int			i, error = 0;

	/*NDIS_LOCK(sc);*/

	switch(command) {
	case SIOCSIFFLAGS:
		if (ifp->if_flags & IFF_UP) {
			if (ifp->if_drv_flags & IFF_DRV_RUNNING &&
			    ifp->if_flags & IFF_PROMISC &&
			    !(sc->ndis_if_flags & IFF_PROMISC)) {
				sc->ndis_filter |=
				    NDIS_PACKET_TYPE_PROMISCUOUS;
				i = sizeof(sc->ndis_filter);
				error = ndis_set_info(sc,
				    OID_GEN_CURRENT_PACKET_FILTER,
				    &sc->ndis_filter, &i);
			} else if (ifp->if_drv_flags & IFF_DRV_RUNNING &&
			    !(ifp->if_flags & IFF_PROMISC) &&
			    sc->ndis_if_flags & IFF_PROMISC) {
				sc->ndis_filter &=
				    ~NDIS_PACKET_TYPE_PROMISCUOUS;
				i = sizeof(sc->ndis_filter);
				error = ndis_set_info(sc,
				    OID_GEN_CURRENT_PACKET_FILTER,
				    &sc->ndis_filter, &i);
			} else
				ndis_init(sc);
		} else {
			if (ifp->if_drv_flags & IFF_DRV_RUNNING)
				ndis_stop(sc);
		}
		sc->ndis_if_flags = ifp->if_flags;
		error = 0;
		break;
	case SIOCADDMULTI:
	case SIOCDELMULTI:
		ndis_setmulti(sc);
		error = 0;
		break;
	case SIOCGIFMEDIA:
	case SIOCSIFMEDIA:
		if (sc->ndis_80211) {
#if __FreeBSD_version < 600007
			error = ieee80211_ioctl(ifp, command, data);
#else
			error = ieee80211_ioctl(&sc->ic, command, data);
#endif
			if (error == ENETRESET) {
				ndis_setstate_80211(sc);
				/*ndis_init(sc);*/
				error = 0;
			}
		} else
			error = ifmedia_ioctl(ifp, ifr, &sc->ifmedia, command);
		break;
	case SIOCSIFCAP:
		ifp->if_capenable = ifr->ifr_reqcap;
		if (ifp->if_capenable & IFCAP_TXCSUM)
			ifp->if_hwassist = sc->ndis_hwassist;
		else
			ifp->if_hwassist = 0;
		ndis_set_offload(sc);
		break;
	case SIOCG80211:
		if (!NDIS_INITIALIZED(sc))
			goto do_80211;
		if (sc->ndis_80211)
			error = ndis_80211_ioctl_get(ifp, command, data);
		else
			error = ENOTTY;
		break;
	case SIOCS80211:
		if (!NDIS_INITIALIZED(sc))
			goto do_80211;
		if (sc->ndis_80211)
			error = ndis_80211_ioctl_set(ifp, command, data);
		else
			error = ENOTTY;
		break;
	case SIOCGDRVSPEC:
		if ((error = priv_check(curthread, PRIV_DRIVER)))
			break;
		error =  copyin(ifr->ifr_data, &oid, sizeof(oid));
		if (error)
			break;
		oidbuf = malloc(oid.len, M_TEMP, M_NOWAIT|M_ZERO);
		if (oidbuf == NULL) {
			error = ENOMEM;
			break;
		}
		error =  copyin(ifr->ifr_data + sizeof(oid), oidbuf, oid.len);
		if (error) {
			free(oidbuf, M_TEMP);
			break;
		}
		error = ndis_get_info(sc, oid.oid, oidbuf, &oid.len);
		if (error) {
			free(oidbuf, M_TEMP);
			break;
		}
		error = copyout(&oid, ifr->ifr_data, sizeof(oid));
		if (error) {
			free(oidbuf, M_TEMP);
			break;
		}
		error = copyout(oidbuf, ifr->ifr_data + sizeof(oid), oid.len);
		free(oidbuf, M_TEMP);
		break;
	case SIOCSDRVSPEC:
		if ((error = priv_check(curthread, PRIV_DRIVER)))
			break;
		error =  copyin(ifr->ifr_data, &oid, sizeof(oid));
		if (error)
			break;
		oidbuf = malloc(oid.len, M_TEMP, M_NOWAIT|M_ZERO);
		if (oidbuf == NULL) {
			error = ENOMEM;
			break;
		}
		error =  copyin(ifr->ifr_data + sizeof(oid), oidbuf, oid.len);
		if (error) {
			free(oidbuf, M_TEMP);
			break;
		}
		error = ndis_set_info(sc, oid.oid, oidbuf, &oid.len);
		if (error) {
			free(oidbuf, M_TEMP);
			break;
		}
		error = copyout(&oid, ifr->ifr_data, sizeof(oid));
		if (error) {
			free(oidbuf, M_TEMP);
			break;
		}
		error = copyout(oidbuf, ifr->ifr_data + sizeof(oid), oid.len);
		free(oidbuf, M_TEMP);
		break;
	case SIOCGPRIVATE_0:
		if ((error = priv_check(curthread, PRIV_DRIVER)))
			break;
		NDIS_LOCK(sc);
		if (sc->ndis_evt[sc->ndis_evtcidx].ne_sts == 0) {
			error = ENOENT;
			NDIS_UNLOCK(sc);
			break;
		}
		error =  copyin(ifr->ifr_data, &evt, sizeof(evt));
		if (error) {
			NDIS_UNLOCK(sc);
			break;
		}
		if (evt.ne_len < sc->ndis_evt[sc->ndis_evtcidx].ne_len) {
			error = ENOSPC;
			NDIS_UNLOCK(sc);
			break;
		}
		error = copyout(&sc->ndis_evt[sc->ndis_evtcidx],
		    ifr->ifr_data, sizeof(uint32_t) * 2);
		if (error) {
			NDIS_UNLOCK(sc);
			break;
		}
		if (sc->ndis_evt[sc->ndis_evtcidx].ne_len) {
			error = copyout(sc->ndis_evt[sc->ndis_evtcidx].ne_buf,
			    ifr->ifr_data + (sizeof(uint32_t) * 2),
			    sc->ndis_evt[sc->ndis_evtcidx].ne_len);
			if (error) {
				NDIS_UNLOCK(sc);
				break;
			}
			free(sc->ndis_evt[sc->ndis_evtcidx].ne_buf, M_TEMP);
			sc->ndis_evt[sc->ndis_evtcidx].ne_buf = NULL;
		}
		sc->ndis_evt[sc->ndis_evtcidx].ne_len = 0;
		sc->ndis_evt[sc->ndis_evtcidx].ne_sts = 0;
		NDIS_EVTINC(sc->ndis_evtcidx);
		NDIS_UNLOCK(sc);
		break;
#if __FreeBSD_version < 700046
	case SIOCGIFGENERIC:
	case SIOCSIFGENERIC:
		if (sc->ndis_80211 && NDIS_INITIALIZED(sc)) {
			if (command == SIOCGIFGENERIC)
				error = ndis_wi_ioctl_get(ifp, command, data);
			else
				error = ndis_wi_ioctl_set(ifp, command, data);
		} else
			error = ENOTTY;
		if (error != ENOTTY)
			break;
#endif
	default:
do_80211:
		sc->ndis_skip = 1;
		if (sc->ndis_80211) {
#if __FreeBSD_version < 600007
			error = ieee80211_ioctl(ifp, command, data);
#else
			error = ieee80211_ioctl(&sc->ic, command, data);
#endif
			if (error == ENETRESET) {
				ndis_setstate_80211(sc);
				error = 0;
			}
		} else
			error = ether_ioctl(ifp, command, data);
		sc->ndis_skip = 0;
		break;
	}

	/*NDIS_UNLOCK(sc);*/

	return(error);
}

#if __FreeBSD_version < 700046
static int
ndis_wi_ioctl_get(ifp, command, data)
	struct ifnet		*ifp;
	u_long			command;
	caddr_t			data;
{
	struct wi_req		wreq;
	struct ifreq		*ifr;
	struct ndis_softc	*sc;
	ndis_80211_bssid_list_ex *bl;
	ndis_wlan_bssid_ex	*wb;
	struct wi_apinfo	*api;
	int			error, i, j, len, maxaps;

	sc = ifp->if_softc;
	ifr = (struct ifreq *)data;
	error = copyin(ifr->ifr_data, &wreq, sizeof(wreq));
	if (error)
		return (error);

	switch (wreq.wi_type) {
	case WI_RID_READ_APS:
		len = 0;
		error = ndis_set_info(sc, OID_802_11_BSSID_LIST_SCAN,
		    NULL, &len);
		if (error == 0)
			tsleep(&error, PPAUSE|PCATCH, "ssidscan", hz * 2);
		len = 0;
		error = ndis_get_info(sc, OID_802_11_BSSID_LIST, NULL, &len);
		if (error != ENOSPC)
			len = 65536;	
		bl = malloc(len, M_DEVBUF, M_NOWAIT|M_ZERO);
		error = ndis_get_info(sc, OID_802_11_BSSID_LIST, bl, &len);
		if (error) {
			free(bl, M_DEVBUF);
			break;
		}
		maxaps = (2 * wreq.wi_len - sizeof(int)) / sizeof(*api);
		maxaps = MIN(maxaps, bl->nblx_items);
		wreq.wi_len = (maxaps * sizeof(*api) + sizeof(int)) / 2;
		*(int *)&wreq.wi_val = maxaps;
		api = (struct wi_apinfo *)&((int *)&wreq.wi_val)[1];
		wb = bl->nblx_bssid;
		while (maxaps--) {
			bzero(api, sizeof(*api));
			bcopy(&wb->nwbx_macaddr, &api->bssid,
			    sizeof(api->bssid));
			api->namelen = wb->nwbx_ssid.ns_ssidlen;
			bcopy(&wb->nwbx_ssid.ns_ssid, &api->name, api->namelen);
			if (wb->nwbx_privacy)
				api->capinfo |= IEEE80211_CAPINFO_PRIVACY;
			/* XXX Where can we get noise information? */
			api->signal = wb->nwbx_rssi + 149;	/* XXX */
			api->quality = api->signal;
			api->channel =
			    ieee80211_mhz2ieee(wb->nwbx_config.nc_dsconfig /
			    1000, 0);
			/* In "auto" infrastructure mode, this is useless. */
			if (wb->nwbx_netinfra == NDIS_80211_NET_INFRA_IBSS)
				api->capinfo |= IEEE80211_CAPINFO_IBSS;
			if (wb->nwbx_len > sizeof(ndis_wlan_bssid)) {
				j = sizeof(ndis_80211_rates_ex);
				/* handle other extended things */
			} else
				j = sizeof(ndis_80211_rates);
			for (i = api->rate = 0; i < j; i++)
				api->rate = MAX(api->rate, 5 *
				    (wb->nwbx_supportedrates[i] & 0x7f));
			api++;
			wb = (ndis_wlan_bssid_ex *)((char *)wb + wb->nwbx_len);
		}
		free(bl, M_DEVBUF);
		error = copyout(&wreq, ifr->ifr_data, sizeof(wreq));
		break;
	default:
		error = ENOTTY;
		break;
	}
	return (error);
}

static int
ndis_wi_ioctl_set(ifp, command, data)
	struct ifnet		*ifp;
	u_long			command;
	caddr_t			data;
{
	struct wi_req		wreq;
	struct ifreq		*ifr;
	struct ndis_softc	*sc;
	uint32_t		foo;
	int			error, len;

	error = priv_check(curthread, PRIV_DRIVER);
	if (error)
		return (error);

	sc = ifp->if_softc;
	ifr = (struct ifreq *)data;
	error = copyin(ifr->ifr_data, &wreq, sizeof(wreq));
	if (error)
		return (error);

	switch (wreq.wi_type) {
	case WI_RID_SCAN_APS:
	case WI_RID_SCAN_REQ:			/* arguments ignored */
		len = sizeof(foo);
		foo = 0;
		error = ndis_set_info(sc, OID_802_11_BSSID_LIST_SCAN, &foo,
		    &len);
		break;
	default:
		error = ENOTTY;
		break;
	}
	return (error);
}
#endif

static int
ndis_80211_ioctl_get(struct ifnet *ifp, u_long command, caddr_t data)
{
	struct ndis_softc	*sc;
	struct ieee80211req	*ireq;
	int			error, len;
	uint16_t		nodename_u[IEEE80211_NWID_LEN + 1];
	unicode_string		us;
	ansi_string		as;

	sc = ifp->if_softc;
	ireq = (struct ieee80211req *) data;

	switch (ireq->i_type) {
#ifdef IEEE80211_IOC_MLME
	case IEEE80211_IOC_MLME:
		error = 0;
		break;
#endif
	case IEEE80211_IOC_STATIONNAME:
		error = ndis_get_info(sc, OID_GEN_MACHINE_NAME,
		    &nodename_u, &len);
		if (error)
			break;
		us.us_len = us.us_maxlen = len;
		us.us_buf = nodename_u;
		if (RtlUnicodeStringToAnsiString(&as, &us, TRUE)) {
			error = ENOMEM;
			break;
		}
		ireq->i_len = as.as_len;
		error = copyout(as.as_buf, ireq->i_data, ireq->i_len);
		RtlFreeAnsiString(&as);
		break;
	default:
#if __FreeBSD_version < 600007
		error = ieee80211_ioctl(ifp, command, data);
#else
		error = ieee80211_ioctl(&sc->ic, command, data);
#endif
		break;
	}
	
	return(error);
}

#ifdef IEEE80211_F_WPA
int
ndis_del_key(ic, key)
	struct ieee80211com	*ic;
	const struct ieee80211_key *key;
{
	struct ndis_softc	*sc;
	ndis_80211_key		rkey;
	int			len, error = 0;

	sc = ic->ic_ifp->if_softc;

	bzero((char *)&rkey, sizeof(rkey));
	len = sizeof(rkey);

	rkey.nk_len = len;
	rkey.nk_keyidx = key->wk_keyix;

	bcopy(ic->ic_ifp->if_broadcastaddr,
	    rkey.nk_bssid, IEEE80211_ADDR_LEN);

	error = ndis_set_info(sc, OID_802_11_REMOVE_KEY, &rkey, &len);

	if (error)
		return(0);

	return(1);
}

/*
 * In theory this could be called for any key, but we'll
 * only use it for WPA TKIP or AES keys. These need to be
 * set after initial authentication with the AP.
 */

static int
ndis_add_key(ic, key, mac)
	struct ieee80211com	*ic;
	const struct ieee80211_key *key;
	const uint8_t		mac[IEEE80211_ADDR_LEN];
{
	struct ndis_softc	*sc;
	ndis_80211_key		rkey;
	int			len, error = 0;

	sc = ic->ic_ifp->if_softc;

	switch (key->wk_cipher->ic_cipher) {
	case IEEE80211_CIPHER_TKIP:

		len = sizeof(ndis_80211_key);
		bzero((char *)&rkey, sizeof(rkey));

		rkey.nk_len = len;
		rkey.nk_keylen = key->wk_keylen;

		if (key->wk_flags & IEEE80211_KEY_SWMIC)
			rkey.nk_keylen += 16;

		/* key index - gets weird in NDIS */

		if (key->wk_keyix != IEEE80211_KEYIX_NONE)
			rkey.nk_keyidx = key->wk_keyix;
		else
			rkey.nk_keyidx = 0;

		if (key->wk_flags & IEEE80211_KEY_XMIT)
			rkey.nk_keyidx |= 1 << 31;

		if (key->wk_flags & IEEE80211_KEY_GROUP) {
			bcopy(ic->ic_ifp->if_broadcastaddr,
			    rkey.nk_bssid, IEEE80211_ADDR_LEN);
		} else {
			bcopy(ic->ic_bss->ni_bssid,
			    rkey.nk_bssid, IEEE80211_ADDR_LEN);
			/* pairwise key */
			rkey.nk_keyidx |= 1 << 30;
		}

		/* need to set bit 29 based on keyrsc */
		rkey.nk_keyrsc = key->wk_keyrsc;

		if (rkey.nk_keyrsc)
			rkey.nk_keyidx |= 1 << 29;

		if (key->wk_flags & IEEE80211_KEY_SWMIC) {
			bcopy(key->wk_key, rkey.nk_keydata, 16);
			bcopy(key->wk_key + 24, rkey.nk_keydata + 16, 8);
			bcopy(key->wk_key + 16, rkey.nk_keydata + 24, 8);
		} else
			bcopy(key->wk_key, rkey.nk_keydata, key->wk_keylen);

		error = ndis_set_info(sc, OID_802_11_ADD_KEY, &rkey, &len);
		break;
	case IEEE80211_CIPHER_WEP:
		error = 0;
		break;
	/*
	 * I don't know how to set up keys for the AES
	 * cipher yet. Is it the same as TKIP?
	 */
	case IEEE80211_CIPHER_AES_CCM:
	default:
		error = ENOTTY;
		break;
	}

	/* We need to return 1 for success, 0 for failure. */

	if (error)
		return(0);

	return (1);
}
#endif

static int
ndis_80211_ioctl_set(struct ifnet *ifp, u_long command, caddr_t data)
{
	struct ndis_softc	*sc;
	struct ieee80211req	*ireq;
	int			error = EINVAL, len;
	ansi_string		as;
	unicode_string		us;

	sc = ifp->if_softc;
	ireq = (struct ieee80211req *) data;

	switch (ireq->i_type) {
#ifdef IEEE80211_IOC_COUNTERMEASURES
	case IEEE80211_IOC_COUNTERMEASURES:
	case IEEE80211_IOC_DROPUNENCRYPTED:
		error = 0;
		break;
#endif
	case IEEE80211_IOC_STATIONNAME:
		error = priv_check(curthread, PRIV_NET80211_MANAGE);
		if (error)
			break;
		if (ireq->i_val != 0 ||
		    ireq->i_len > IEEE80211_NWID_LEN) {
			error = EINVAL;
			break;
		}
		as.as_len = as.as_maxlen = ireq->i_len;
		as.as_buf = ireq->i_data;
		if (RtlAnsiStringToUnicodeString(&us, &as, TRUE)) {
			error = ENOMEM;
			break;
		}
		len = us.us_len;
		error = ndis_set_info(sc, OID_GEN_MACHINE_NAME,
		    us.us_buf, &len);
		RtlFreeUnicodeString(&us);
		break;
	default:
#if __FreeBSD_version < 600007
		error = ieee80211_ioctl(ifp, command, data);
#else
		error = ieee80211_ioctl(&sc->ic, command, data);
#endif
		if (error == ENETRESET) {
			ndis_setstate_80211(sc);
			error = 0;
		}
	}
	
	return(error);
}

static void
ndis_resettask(d, arg)
	device_object		*d;
	void			*arg;
{
	struct ndis_softc		*sc;

	sc = arg;
	ndis_reset_nic(sc);
	return;
}

static void
ndis_watchdog(ifp)
	struct ifnet		*ifp;
{
	struct ndis_softc		*sc;

	sc = ifp->if_softc;

	NDIS_LOCK(sc);
	ifp->if_oerrors++;
	device_printf(sc->ndis_dev, "watchdog timeout\n");
	NDIS_UNLOCK(sc);

	IoQueueWorkItem(sc->ndis_resetitem,
	    (io_workitem_func)ndis_resettask_wrap,
	    WORKQUEUE_CRITICAL, sc);
	IoQueueWorkItem(sc->ndis_startitem,
	    (io_workitem_func)ndis_starttask_wrap,
	    WORKQUEUE_CRITICAL, ifp);

	return;
}

/*
 * Stop the adapter and free any mbufs allocated to the
 * RX and TX lists.
 */
static void
ndis_stop(sc)
	struct ndis_softc		*sc;
{
	struct ifnet		*ifp;
	struct ieee80211com	*ic;
	int			i;

	ic = &sc->ic;
	if (sc->ndis_80211)
		ieee80211_new_state(ic, IEEE80211_S_INIT, -1);

	ifp = sc->ifp;
	untimeout(ndis_tick, sc, sc->ndis_stat_ch);

	NDIS_LOCK(sc);
	ifp->if_timer = 0;
	sc->ndis_link = 0;
	ifp->if_drv_flags &= ~(IFF_DRV_RUNNING | IFF_DRV_OACTIVE);
	NDIS_UNLOCK(sc);

	ndis_halt_nic(sc);

	NDIS_LOCK(sc);
	for (i = 0; i < NDIS_EVENTS; i++) {
		if (sc->ndis_evt[i].ne_sts && sc->ndis_evt[i].ne_buf != NULL)
			free(sc->ndis_evt[i].ne_buf, M_TEMP);
		sc->ndis_evt[i].ne_sts = 0;
		sc->ndis_evt[i].ne_len = 0;
	}
	sc->ndis_evtcidx = 0;
	sc->ndis_evtpidx = 0;
	NDIS_UNLOCK(sc);

	return;
}

/*
 * Stop all chip I/O so that the kernel's probe routines don't
 * get confused by errant DMAs when rebooting.
 */
void
ndis_shutdown(dev)
	device_t		dev;
{
	struct ndis_softc		*sc;

	sc = device_get_softc(dev);
	ndis_stop(sc);

	return;
}

static int
ndis_newstate(struct ieee80211com *ic, enum ieee80211_state nstate, int arg)
{
	struct ifnet *ifp = ic->ic_ifp;
	struct ndis_softc *sc = ifp->if_softc;
	 enum ieee80211_state ostate;

	DPRINTF(("%s: %s -> %s\n", __func__,
		ieee80211_state_name[ic->ic_state],
		ieee80211_state_name[nstate]));

	ostate = ic->ic_state;
	ic->ic_state = nstate;

	switch (nstate) {
	/* pass on to net80211 */
	case IEEE80211_S_INIT:
	case IEEE80211_S_SCAN:
		return (sc->ndis_newstate(ic, nstate, arg));

	case IEEE80211_S_ASSOC:
		if (ostate != IEEE80211_S_AUTH)
			ndis_setstate_80211(sc);
		break;

	case IEEE80211_S_AUTH:
		ndis_setstate_80211(sc);
		break;

	default:
		break;
	}
	return (0);
}

static void
ndis_scan(void *arg, int npending)
{
	struct ndis_softc *sc = arg;
	struct ieee80211com *ic = (void *)&sc->ic;
	int error, len;

	len = 0;
	error = ndis_set_info(sc, OID_802_11_BSSID_LIST_SCAN,
	    NULL, &len);
	tsleep(&error, PPAUSE|PCATCH, "ssidscan", hz * 2);
	ndis_scan_results(sc);
	ieee80211_scan_done(ic);
}

static void
ndis_scan_results(struct ndis_softc *sc)
{
	struct ieee80211com *ic = (void *)&sc->ic;
	ndis_80211_bssid_list_ex *bl;
	ndis_wlan_bssid_ex	*wb;
	struct ieee80211_scanparams sp;
	struct ieee80211_frame wh;
	int i, j;
	int error, len, rssi, noise;
	static long rstamp;
	uint8_t ssid[2+IEEE80211_NWID_LEN];
	uint8_t rates[2+IEEE80211_RATE_MAXSIZE];
	uint8_t *frm, *efrm;

	len = 0;
	noise = -96;
	error = ndis_get_info(sc, OID_802_11_BSSID_LIST, NULL, &len);
	if (error != ENOSPC)
		len = 65536;
	bl = malloc(len, M_DEVBUF, M_NOWAIT | M_ZERO);
	error = ndis_get_info(sc, OID_802_11_BSSID_LIST, bl, &len);
	if (error) {
		free(bl, M_DEVBUF);
		return;;
	}

	rstamp++;
	wb = &bl->nblx_bssid[0];
	for (i = 0; i < bl->nblx_items; i++) {
		memset(&sp, 0, sizeof(sp));

		memcpy(wh.i_addr2, wb->nwbx_macaddr, sizeof(wh.i_addr2));
		memcpy(wh.i_addr3, wb->nwbx_macaddr, sizeof(wh.i_addr3));
		rssi = (wb->nwbx_rssi - noise) / (-32 - noise) * 100;
		rssi = max(0, min(rssi, 100));	/* limit 0 <= rssi <= 100 */
		if (wb->nwbx_privacy)
			sp.capinfo |= IEEE80211_CAPINFO_PRIVACY;
		sp.bintval = wb->nwbx_config.nc_beaconperiod;
		switch (wb->nwbx_netinfra) {
			case NDIS_80211_NET_INFRA_IBSS:
				sp.capinfo |= IEEE80211_CAPINFO_IBSS;
				break;
			case NDIS_80211_NET_INFRA_BSS:
				sp.capinfo |= IEEE80211_CAPINFO_ESS;
				break;
		}
		sp.rates = &rates[0];
		for (j = 0; j < IEEE80211_RATE_MAXSIZE; j++) {
			/* XXX - check units */
			if (wb->nwbx_supportedrates[j] == 0)
				break;
			rates[2 + j] =
			wb->nwbx_supportedrates[j] & 0x7f;
		}
		rates[1] = j;
		sp.ssid = (uint8_t *)&ssid[0];
		memcpy(sp.ssid + 2, &wb->nwbx_ssid.ns_ssid,
		    wb->nwbx_ssid.ns_ssidlen);
		sp.ssid[1] = wb->nwbx_ssid.ns_ssidlen;

		sp.bchan = ieee80211_mhz2ieee(
		    wb->nwbx_config.nc_dsconfig / 1000, 0);
		sp.curchan = ieee80211_find_channel(ic,
			ieee80211_ieee2mhz(sp.bchan, IEEE80211_CHAN_B),
			IEEE80211_CHAN_B);
		if (sp.curchan == NULL)
			sp.curchan = &ic->ic_channels[0];

		/* Process extended info from AP */
		if (wb->nwbx_len > sizeof(ndis_wlan_bssid)) {
			frm = (uint8_t *)&wb->nwbx_ies;
			efrm = frm + wb->nwbx_ielen;
			if (efrm - frm < 12)
				goto done;
			sp.tstamp = frm;
			frm += 8;
			sp.bintval = le16toh(*(uint16_t *)frm);
			frm += 2;
			sp.capinfo = le16toh(*(uint16_t *)frm);
			frm += 2;

			/* Grab variable length ies */
			while (efrm - frm > 1) {
				if (efrm - frm < frm[1] + 2)
					break;
				switch (*frm) {
				case IEEE80211_ELEMID_RSN:
					sp.rsn = frm;
					break;
				}
				frm += frm[1] + 2;
			}
		}
done:
		ieee80211_add_scan(ic, &sp, &wh, 0, rssi, noise, rstamp);
		wb = (ndis_wlan_bssid_ex *)((char *)wb + wb->nwbx_len);
	}
}

static void
ndis_scan_start(struct ieee80211com *ic)
{
	struct ifnet *ifp = ic->ic_ifp;
	struct ndis_softc *sc = ifp->if_softc;

	taskqueue_enqueue(sc->ndis_tq, &sc->ndis_scantask);
}

static void
ndis_set_channel(struct ieee80211com *ic)
{
	/* ignore */
}

static void
ndis_scan_curchan(struct ieee80211com *ic, unsigned long maxdwell)
{
	/* ignore */
}

static void
ndis_scan_mindwell(struct ieee80211com *ic)
{
	/* NB: don't try to abort scan; wait for firmware to finish */
}

static void
ndis_scan_end(struct ieee80211com *ic)
{
	/* ignore */
}

