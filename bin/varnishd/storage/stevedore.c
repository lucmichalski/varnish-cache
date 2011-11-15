/*-
 * Copyright (c) 2007-2011 Varnish Software AS
 * All rights reserved.
 *
 * Author: Dag-Erling Smørgav <des@des.no>
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
 * ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * STEVEDORE: one who works at or is responsible for loading and
 * unloading ships in port.  Example: "on the wharves, stevedores were
 * unloading cargo from the far corners of the world." Origin: Spanish
 * estibador, from estibar to pack.  First Known Use: 1788
 */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>

#include "cache/cache.h"

#include "storage/storage.h"
#include "vcli_priv.h"
#include "vrt.h"
#include "vrt_obj.h"

static const struct stevedore * volatile stv_next;

/*---------------------------------------------------------------------
 * Default objcore methods
 */

static struct object * __match_proto__(getobj_f)
default_oc_getobj(struct worker *wrk, struct objcore *oc)
{
	struct object *o;

	(void)wrk;
	if (oc->priv == NULL)
		return (NULL);
	CAST_OBJ_NOTNULL(o, oc->priv, OBJECT_MAGIC);
	return (o);
}

static void
default_oc_freeobj(struct objcore *oc)
{
	struct object *o;

	CAST_OBJ_NOTNULL(o, oc->priv, OBJECT_MAGIC);
	oc->priv = NULL;
	oc->methods = NULL;

	STV_Freestore(o);
	STV_free(o->objstore);
}

static struct lru *
default_oc_getlru(const struct objcore *oc)
{
	struct object *o;

	CAST_OBJ_NOTNULL(o, oc->priv, OBJECT_MAGIC);
	return (o->objstore->stevedore->lru);
}

static struct objcore_methods default_oc_methods = {
	.getobj = default_oc_getobj,
	.freeobj = default_oc_freeobj,
	.getlru = default_oc_getlru,
};


/*--------------------------------------------------------------------
 */

struct lru *
LRU_Alloc(void)
{
	struct lru *l;

	ALLOC_OBJ(l, LRU_MAGIC);
	AN(l);
	VTAILQ_INIT(&l->lru_head);
	Lck_New(&l->mtx, lck_lru);
	return (l);
}

void
LRU_Free(struct lru *lru)
{
	CHECK_OBJ_NOTNULL(lru, LRU_MAGIC);
	Lck_Delete(&lru->mtx);
	FREE_OBJ(lru);
}

/*--------------------------------------------------------------------
 * XXX: trust pointer writes to be atomic
 */

static struct stevedore *
stv_pick_stevedore(const struct sess *sp, const char **hint)
{
	struct stevedore *stv;

	AN(hint);
	if (*hint != NULL && **hint != '\0') {
		VTAILQ_FOREACH(stv, &stv_stevedores, list) {
			if (!strcmp(stv->ident, *hint))
				return (stv);
		}
		if (!strcmp(TRANSIENT_STORAGE, *hint))
			return (stv_transient);

		/* Hint was not valid, nuke it */
		WSP(sp, SLT_Debug, "Storage hint not usable");
		*hint = NULL;
	}
	/* pick a stevedore and bump the head along */
	stv = VTAILQ_NEXT(stv_next, list);
	if (stv == NULL)
		stv = VTAILQ_FIRST(&stv_stevedores);
	AN(stv);
	AN(stv->name);
	stv_next = stv;
	return (stv);
}

/*-------------------------------------------------------------------*/

static struct storage *
stv_alloc(struct worker *w, const struct object *obj, size_t size)
{
	struct storage *st;
	struct stevedore *stv;
	unsigned fail = 0;

	/*
	 * Always use the stevedore which allocated the object in order to
	 * keep an object inside the same stevedore.
	 */
	CHECK_OBJ_NOTNULL(obj, OBJECT_MAGIC);
	CHECK_OBJ_NOTNULL(w, WORKER_MAGIC);
	stv = obj->objstore->stevedore;
	CHECK_OBJ_NOTNULL(stv, STEVEDORE_MAGIC);

	if (size > (size_t)(cache_param->fetch_maxchunksize) << 10)
		size = (size_t)(cache_param->fetch_maxchunksize) << 10;

	for (;;) {
		/* try to allocate from it */
		AN(stv->alloc);
		st = stv->alloc(stv, size);
		if (st != NULL)
			break;

		if (size > cache_param->fetch_chunksize * 1024LL) {
			size >>= 1;
			continue;
		}

		/* no luck; try to free some space and keep trying */
		if (EXP_NukeOne(w, stv->lru) == -1)
			break;

		/* Enough is enough: try another if we have one */
		if (++fail >= cache_param->nuke_limit)
			break;
	}
	if (st != NULL)
		CHECK_OBJ_NOTNULL(st, STORAGE_MAGIC);
	return (st);
}


/*-------------------------------------------------------------------*
 * Structure used to transport internal knowledge from STV_NewObject()
 * to STV_MkObject().  Nobody else should mess with this struct.
 */

struct stv_objsecrets {
	unsigned	magic;
#define STV_OBJ_SECRETES_MAGIC	0x78c87247
	uint16_t	nhttp;
	unsigned	lhttp;
	unsigned	wsl;
	struct exp	*exp;
};

/*--------------------------------------------------------------------
 * This function is called by stevedores ->allocobj() method, which
 * very often will be stv_default_allocobj() below, to convert a slab
 * of storage into object which the stevedore can then register in its
 * internal state, before returning it to STV_NewObject().
 * As you probably guessed: All this for persistence.
 */

struct object *
STV_MkObject(struct sess *sp, void *ptr, unsigned ltot,
    const struct stv_objsecrets *soc)
{
	struct object *o;
	unsigned l;

	CHECK_OBJ_NOTNULL(soc, STV_OBJ_SECRETES_MAGIC);

	assert(PAOK(ptr));
	assert(PAOK(soc->wsl));
	assert(PAOK(soc->lhttp));

	assert(ltot >= sizeof *o + soc->lhttp + soc->wsl);

	o = ptr;
	memset(o, 0, sizeof *o);
	o->magic = OBJECT_MAGIC;

	l = PRNDDN(ltot - (sizeof *o + soc->lhttp));
	assert(l >= soc->wsl);

	o->http = HTTP_create(o + 1, soc->nhttp);
	WS_Init(o->ws_o, "obj", (char *)(o + 1) + soc->lhttp, soc->wsl);
	WS_Assert(o->ws_o);
	assert(o->ws_o->e <= (char*)ptr + ltot);

	http_Setup(o->http, o->ws_o);
	o->http->magic = HTTP_MAGIC;
	o->exp = *soc->exp;
	VTAILQ_INIT(&o->store);
	sp->wrk->stats.n_object++;

	if (sp->objcore != NULL) {
		CHECK_OBJ_NOTNULL(sp->objcore, OBJCORE_MAGIC);

		o->objcore = sp->objcore;
		sp->objcore = NULL;     /* refcnt follows pointer. */
		BAN_NewObjCore(o->objcore);

		o->objcore->methods = &default_oc_methods;
		o->objcore->priv = o;
	}
	return (o);
}

/*--------------------------------------------------------------------
 * This is the default ->allocobj() which all stevedores who do not
 * implement persistent storage can rely on.
 */

struct object *
stv_default_allocobj(struct stevedore *stv, struct sess *sp, unsigned ltot,
    const struct stv_objsecrets *soc)
{
	struct object *o;
	struct storage *st;

	CHECK_OBJ_NOTNULL(soc, STV_OBJ_SECRETES_MAGIC);
	st = stv->alloc(stv, ltot);
	if (st == NULL)
		return (NULL);
	if (st->space < ltot) {
		stv->free(st);
		return (NULL);
	}
	ltot = st->len = st->space;
	o = STV_MkObject(sp, st->ptr, ltot, soc);
	CHECK_OBJ_NOTNULL(o, OBJECT_MAGIC);
	o->objstore = st;
	return (o);
}

/*-------------------------------------------------------------------
 * Allocate storage for an object, based on the header information.
 * XXX: If we know (a hint of) the length, we could allocate space
 * XXX: for the body in the same allocation while we are at it.
 */

struct object *
STV_NewObject(struct sess *sp, const char *hint, unsigned wsl, struct exp *ep,
    uint16_t nhttp)
{
	struct object *o;
	struct stevedore *stv, *stv0;
	unsigned lhttp, ltot;
	struct stv_objsecrets soc;
	int i;

	assert(wsl > 0);
	wsl = PRNDUP(wsl);

	lhttp = HTTP_estimate(nhttp);
	lhttp = PRNDUP(lhttp);

	memset(&soc, 0, sizeof soc);
	soc.magic = STV_OBJ_SECRETES_MAGIC;
	soc.nhttp = nhttp;
	soc.lhttp = lhttp;
	soc.wsl = wsl;
	soc.exp = ep;

	ltot = sizeof *o + wsl + lhttp;

	stv = stv0 = stv_pick_stevedore(sp, &hint);
	AN(stv->allocobj);
	o = stv->allocobj(stv, sp, ltot, &soc);
	if (o == NULL && hint == NULL) {
		do {
			stv = stv_pick_stevedore(sp, &hint);
			AN(stv->allocobj);
			o = stv->allocobj(stv, sp, ltot, &soc);
		} while (o == NULL && stv != stv0);
	}
	if (o == NULL) {
		/* no luck; try to free some space and keep trying */
		for (i = 0; o == NULL && i < cache_param->nuke_limit; i++) {
			if (EXP_NukeOne(sp->wrk, stv->lru) == -1)
				break;
			o = stv->allocobj(stv, sp, ltot, &soc);
		}
	}

	if (o == NULL)
		return (NULL);
	CHECK_OBJ_NOTNULL(o, OBJECT_MAGIC);
	CHECK_OBJ_NOTNULL(o->objstore, STORAGE_MAGIC);
	return (o);
}

/*-------------------------------------------------------------------*/

void
STV_Freestore(struct object *o)
{
	struct storage *st, *stn;

	if (o->esidata != NULL) {
		STV_free(o->esidata);
		o->esidata = NULL;
	}
	VTAILQ_FOREACH_SAFE(st, &o->store, list, stn) {
		CHECK_OBJ_NOTNULL(st, STORAGE_MAGIC);
		VTAILQ_REMOVE(&o->store, st, list);
		STV_free(st);
	}
}

/*-------------------------------------------------------------------*/

struct storage *
STV_alloc(struct worker *w, size_t size)
{

	return (stv_alloc(w, w->fetch_obj, size));
}

void
STV_trim(struct storage *st, size_t size)
{

	CHECK_OBJ_NOTNULL(st, STORAGE_MAGIC);
	AN(st->stevedore);
	if (st->stevedore->trim)
		st->stevedore->trim(st, size);
}

void
STV_free(struct storage *st)
{

	CHECK_OBJ_NOTNULL(st, STORAGE_MAGIC);
	AN(st->stevedore);
	AN(st->stevedore->free);
	st->stevedore->free(st);
}

void
STV_open(void)
{
	struct stevedore *stv;

	VTAILQ_FOREACH(stv, &stv_stevedores, list) {
		stv->lru = LRU_Alloc();
		if (stv->open != NULL)
			stv->open(stv);
	}
	stv = stv_transient;
	if (stv->open != NULL) {
		stv->lru = LRU_Alloc();
		stv->open(stv);
	}
	stv_next = VTAILQ_FIRST(&stv_stevedores);
}

void
STV_close(void)
{
	struct stevedore *stv;

	VTAILQ_FOREACH(stv, &stv_stevedores, list)
		if (stv->close != NULL)
			stv->close(stv);
	stv = stv_transient;
	if (stv->close != NULL)
		stv->close(stv);
}

/*--------------------------------------------------------------------*/

static void
stv_cli_list(struct cli *cli, const char * const *av, void *priv)
{
	struct stevedore *stv;

	ASSERT_MGT();
	(void)av;
	(void)priv;
	VCLI_Out(cli, "Storage devices:\n");
	stv = stv_transient;
		VCLI_Out(cli, "\tstorage.%s = %s\n", stv->ident, stv->name);
	VTAILQ_FOREACH(stv, &stv_stevedores, list)
		VCLI_Out(cli, "\tstorage.%s = %s\n", stv->ident, stv->name);
}

/*--------------------------------------------------------------------*/

struct cli_proto cli_stv[] = {
	{ "storage.list", "storage.list", "List storage devices\n",
	    0, 0, "", stv_cli_list },
	{ NULL}
};

/*--------------------------------------------------------------------
 * VRT functions for stevedores
 */

static const struct stevedore *
stv_find(const char *nm)
{
	const struct stevedore *stv;

	VTAILQ_FOREACH(stv, &stv_stevedores, list)
		if (!strcmp(stv->ident, nm))
			return (stv);
	if (!strcmp(TRANSIENT_STORAGE, nm))
		return (stv_transient);
	return (NULL);
}

int
VRT_Stv(const char *nm)
{

	if (stv_find(nm) != NULL)
		return (1);
	return (0);
}

#define VRTSTVVAR(nm, vtype, ctype, dval)	\
ctype						\
VRT_Stv_##nm(const char *nm)			\
{						\
	const struct stevedore *stv;		\
						\
	stv = stv_find(nm);			\
	if (stv == NULL)			\
		return (dval);			\
	if (stv->var_##nm == NULL)		\
		return (dval);			\
	return (stv->var_##nm(stv));		\
}

#include "tbl/vrt_stv_var.h"
#undef VRTSTVVAR
