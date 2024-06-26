/*-
 * Copyright (c) 2023 Juniper Networks, Inc.
 * All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/cdefs.h>
#include <sys/types.h>
#include <sys/bus.h>
#include "tpm_if.h"
#include "tpm20.h"

/* Override accessors */
static uint8_t
tpm_read_1(device_t dev, bus_size_t off)
{
	struct tpm_sc *sc = device_get_softc(dev);

	return (bus_read_1(sc->mem_res, off));
}

static uint32_t
tpm_read_4(device_t dev, bus_size_t off)
{
	struct tpm_sc *sc = device_get_softc(dev);

	return (bus_read_4(sc->mem_res, off));
}

/*
 * Only i386 is missing bus_space_read_8.
 */
#ifndef __i386__
static uint64_t
tpm_read_8(device_t dev, bus_size_t off)
{
	struct tpm_sc *sc = device_get_softc(dev);

	return (bus_read_8(sc->mem_res, off));
}
#endif

static void
tpm_write_1(device_t dev, bus_size_t off, uint8_t val)
{
	struct tpm_sc *sc = device_get_softc(dev);

	bus_write_1(sc->mem_res, off, val);
}

static void
tpm_write_4(device_t dev, bus_size_t off, uint32_t val)
{
	struct tpm_sc *sc = device_get_softc(dev);

	bus_write_4(sc->mem_res, off, val);
}

static void
tpm_write_barrier(device_t dev, bus_addr_t off, bus_size_t length)
{
	struct tpm_sc *sc = device_get_softc(dev);

	bus_barrier(sc->mem_res, off, length, BUS_SPACE_BARRIER_WRITE);
}

static device_method_t tpm_bus_methods[] = {
	DEVMETHOD(tpm_read_1,	tpm_read_1),
	DEVMETHOD(tpm_read_4,	tpm_read_4),
#ifndef __i386__
	DEVMETHOD(tpm_read_8,	tpm_read_8),
#endif
	DEVMETHOD(tpm_write_1,	tpm_write_1),
	DEVMETHOD(tpm_write_4,	tpm_write_4),
	DEVMETHOD(tpm_write_barrier,	tpm_write_barrier),
	DEVMETHOD_END
};

DEFINE_CLASS_0(tpm_lbc, tpm_bus_driver, tpm_bus_methods, sizeof(struct tpm_sc));
