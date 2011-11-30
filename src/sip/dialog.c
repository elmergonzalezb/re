/**
 * @file dialog.c  SIP Dialog
 *
 * Copyright (C) 2010 Creytiv.com
 */
#include <string.h>
#include <re_types.h>
#include <re_mem.h>
#include <re_mbuf.h>
#include <re_sa.h>
#include <re_list.h>
#include <re_hash.h>
#include <re_fmt.h>
#include <re_uri.h>
#include <re_sys.h>
#include <re_tmr.h>
#include <re_udp.h>
#include <re_sip.h>
#include "sip.h"


enum {
	ROUTE_OFFSET = 7,
	X64_STRSIZE = 17,
};

struct sip_dialog {
	struct uri route;
	struct mbuf *mb;
	char *callid;
	char *ltag;
	char *rtag;
	char *uri;
	uint32_t lseq;
	uint32_t rseq;
	size_t cpos;
};


struct route_enc {
	struct mbuf *mb;
	size_t end;
};


static int x64_strdup(char **strp, uint64_t val)
{
	char *str;

	str = mem_alloc(X64_STRSIZE, NULL);
	if (!str)
		return ENOMEM;

	(void)re_snprintf(str, X64_STRSIZE, "%016llx", val);

	*strp = str;

	return 0;
}


static void destructor(void *arg)
{
	struct sip_dialog *dlg = arg;

	mem_deref(dlg->callid);
	mem_deref(dlg->ltag);
	mem_deref(dlg->rtag);
	mem_deref(dlg->uri);
	mem_deref(dlg->mb);
}


int sip_dialog_alloc(struct sip_dialog **dlgp,
		     const char *uri, const char *to_uri,
		     const char *from_name, const char *from_uri,
		     const char *routev[], uint32_t routec)
{
	const uint64_t ltag = rand_u64();
	struct sip_dialog *dlg;
	struct sip_addr addr;
	size_t rend = 0;
	struct pl pl;
	uint32_t i;
	int err;

	if (!dlgp || !uri || !to_uri || !from_uri)
		return EINVAL;

	dlg = mem_zalloc(sizeof(*dlg), destructor);
	if (!dlg)
		return ENOMEM;

	dlg->lseq = rand_u16();

	err = str_dup(&dlg->uri, uri);
	if (err)
		goto out;

	err = x64_strdup(&dlg->callid, rand_u64());
	if (err)
		goto out;

	err = x64_strdup(&dlg->ltag, ltag);
	if (err)
		goto out;

	dlg->mb = mbuf_alloc(512);
	if (!dlg->mb) {
		err = ENOMEM;
		goto out;
	}

	for (i=0; i<routec; i++) {
		err |= mbuf_printf(dlg->mb, "Route: <%s;lr>\r\n", routev[i]);
		if (i == 0)
			rend = dlg->mb->pos - 2;
	}
	err |= mbuf_printf(dlg->mb, "To: <%s>\r\n", to_uri);
	dlg->cpos = dlg->mb->pos;
	err |= mbuf_printf(dlg->mb, "From: %s%s%s<%s>;tag=%016llx\r\n",
			   from_name ? "\"" : "", from_name,
			   from_name ? "\" " : "",
			   from_uri, ltag);
	if (err)
		goto out;

	dlg->mb->pos = 0;

	if (rend) {
		pl.p = (const char *)mbuf_buf(dlg->mb) + ROUTE_OFFSET;
		pl.l = rend - ROUTE_OFFSET;
		err = sip_addr_decode(&addr, &pl);
		dlg->route = addr.uri;
	}
	else {
		pl_set_str(&pl, dlg->uri);
		err = uri_decode(&dlg->route, &pl);
	}

 out:
	if (err)
		mem_deref(dlg);
	else
		*dlgp = dlg;

	return err;
}


static bool record_route_handler(const struct sip_hdr *hdr,
				 const struct sip_msg *msg,
				 void *arg)
{
	struct route_enc *renc = arg;
	(void)msg;

	if (mbuf_printf(renc->mb, "Route: %r\r\n", &hdr->val))
		return true;

	if (!renc->end)
	        renc->end = renc->mb->pos - 2;

	return false;
}


int sip_dialog_accept(struct sip_dialog **dlgp, const struct sip_msg *msg)
{
	const struct sip_hdr *contact;
	struct sip_dialog *dlg;
	struct route_enc renc;
	struct sip_addr addr;
	struct pl pl;
	int err;

	if (!dlgp || !msg || !msg->req)
		return EINVAL;

	contact = sip_msg_hdr(msg, SIP_HDR_CONTACT);

	if (!contact || !msg->callid.p)
		return EBADMSG;

	if (sip_addr_decode(&addr, &contact->val))
		return EBADMSG;

	dlg = mem_zalloc(sizeof(*dlg), destructor);
	if (!dlg)
		return ENOMEM;

	dlg->lseq = rand_u16();
	dlg->rseq = msg->cseq.num;

	err = pl_strdup(&dlg->uri, &addr.auri);
	if (err)
		goto out;

	err = pl_strdup(&dlg->callid, &msg->callid);
	if (err)
		goto out;

	err = x64_strdup(&dlg->ltag, msg->tag);
	if (err)
		goto out;

	err = pl_strdup(&dlg->rtag, &msg->from.tag);
	if (err)
		goto out;

	dlg->mb = mbuf_alloc(512);
	if (!dlg->mb) {
		err = ENOMEM;
		goto out;
	}

	renc.mb  = dlg->mb;
	renc.end = 0;

	err |= sip_msg_hdr_apply(msg, true, SIP_HDR_RECORD_ROUTE,
				 record_route_handler, &renc) ? ENOMEM : 0;
	dlg->cpos = dlg->mb->pos;
	err |= mbuf_printf(dlg->mb, "To: %r\r\n", &msg->from.val);
	err |= mbuf_printf(dlg->mb, "From: %r;tag=%016llx\r\n", &msg->to.val,
			   msg->tag);
	if (err)
		goto out;

	dlg->mb->pos = 0;

	if (renc.end) {
		pl.p = (const char *)mbuf_buf(dlg->mb) + ROUTE_OFFSET;
		pl.l = renc.end - ROUTE_OFFSET;
		err = sip_addr_decode(&addr, &pl);
		dlg->route = addr.uri;
	}
	else {
		pl_set_str(&pl, dlg->uri);
		err = uri_decode(&dlg->route, &pl);
	}

 out:
	if (err)
		mem_deref(dlg);
	else
		*dlgp = dlg;

	return err;
}


int sip_dialog_create(struct sip_dialog *dlg, const struct sip_msg *msg)
{
	const struct sip_hdr *contact;
	struct route_enc renc;
	struct sip_addr addr;
	struct pl pl;
	int err;

	if (!dlg || dlg->rtag || !msg)
		return EINVAL;

	contact = sip_msg_hdr(msg, SIP_HDR_CONTACT);

	if (!contact)
		return EBADMSG;

	if (sip_addr_decode(&addr, &contact->val))
		return EBADMSG;

	dlg->uri = mem_deref(dlg->uri);

	err = pl_strdup(&dlg->uri, &addr.auri);
	if (err)
		return err;

	err = pl_strdup(&dlg->rtag, msg->req ? &msg->from.tag : &msg->to.tag);
	if (err)
		return err;

	renc.mb = mbuf_alloc(512);
	if (!renc.mb)
		return ENOMEM;

	renc.end = 0;

	err |= sip_msg_hdr_apply(msg, msg->req, SIP_HDR_RECORD_ROUTE,
				 record_route_handler, &renc) ? ENOMEM : 0;
	err |= mbuf_printf(renc.mb, "To: %r\r\n",
			   msg->req ? &msg->from.val : &msg->to.val);

	dlg->mb->pos = dlg->cpos;
	err |= mbuf_write_mem(renc.mb, mbuf_buf(dlg->mb),
			      mbuf_get_left(dlg->mb));
	dlg->mb->pos = 0;

	if (err)
		goto out;

	renc.mb->pos = 0;

	if (renc.end) {
		pl.p = (const char *)mbuf_buf(renc.mb) + ROUTE_OFFSET;
		pl.l = renc.end - ROUTE_OFFSET;
		err = sip_addr_decode(&addr, &pl);
		dlg->route = addr.uri;
	}
	else {
		pl_set_str(&pl, dlg->uri);
		err = uri_decode(&dlg->route, &pl);
	}

	if (err)
		goto out;

	mem_deref(dlg->mb);
	dlg->mb = mem_ref(renc.mb);

 out:
	mem_deref(renc.mb);

	return err;
}


int sip_dialog_update(struct sip_dialog *dlg, const struct sip_msg *msg)
{
	const struct sip_hdr *contact;
	struct sip_addr addr;
	struct pl pl;
	char *uri;
	int err;

	if (!dlg || !msg)
		return EINVAL;

	contact = sip_msg_hdr(msg, SIP_HDR_CONTACT);
	if (!contact)
		return EBADMSG;

	if (sip_addr_decode(&addr, &contact->val))
		return EBADMSG;

	err = pl_strdup(&uri, &addr.auri);
	if (err)
		return err;

	if (dlg->route.scheme.p == dlg->uri) {

		pl_set_str(&pl, uri);
		err = uri_decode(&dlg->route, &pl);
		if (err)
			goto out;
	}

	mem_deref(dlg->uri);
	dlg->uri = mem_ref(uri);

 out:
	mem_deref(uri);

	return err;
}


bool sip_dialog_rseq_valid(struct sip_dialog *dlg, const struct sip_msg *msg)
{
	if (!dlg || !msg || !msg->req)
		return false;

	if (msg->cseq.num < dlg->rseq)
		return false;

	dlg->rseq = msg->cseq.num;

	return true;
}


int sip_dialog_encode(struct mbuf *mb, struct sip_dialog *dlg, uint32_t cseq,
		      const char *met)
{
	int err = 0;

	if (!mb || !dlg || !met)
		return EINVAL;

	err |= mbuf_write_mem(mb, mbuf_buf(dlg->mb), mbuf_get_left(dlg->mb));
	err |= mbuf_printf(mb, "Call-ID: %s\r\n", dlg->callid);
	err |= mbuf_printf(mb, "CSeq: %u %s\r\n", strcmp(met, "ACK") ?
			   dlg->lseq++ : cseq, met);

	return err;
}


const char *sip_dialog_uri(const struct sip_dialog *dlg)
{
	return dlg ? dlg->uri : NULL;
}


const struct uri *sip_dialog_route(const struct sip_dialog *dlg)
{
	return dlg ? &dlg->route : NULL;
}


const char *sip_dialog_callid(const struct sip_dialog *dlg)
{
	return dlg ? dlg->callid : NULL;
}


bool sip_dialog_cmp(const struct sip_dialog *dlg, const struct sip_msg *msg)
{
	if (!dlg || !msg)
		return false;

	if (pl_strcmp(&msg->callid, dlg->callid))
		return false;

	if (pl_strcmp(msg->req ? &msg->to.tag : &msg->from.tag, dlg->ltag))
		return false;

	if (pl_strcmp(msg->req ? &msg->from.tag : &msg->to.tag, dlg->rtag))
		return false;

	return true;
}


bool sip_dialog_cmp_half(const struct sip_dialog *dlg,
			 const struct sip_msg *msg)
{
	if (!dlg || !msg)
		return false;

	if (pl_strcmp(&msg->callid, dlg->callid))
		return false;

	if (pl_strcmp(msg->req ? &msg->to.tag : &msg->from.tag, dlg->ltag))
		return false;

	if (dlg->rtag)
		return false;

	return true;
}
