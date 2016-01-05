#include <stdint.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <osmocom/core/select.h>
#include <osmocom/core/prim.h>
#include <osmocom/core/talloc.h>
#include <osmocom/core/logging.h>
#include <osmocom/core/application.h>
#include <osmocom/vty/logging.h>

#include <osmocom/gsm/gsm48.h>
#include <osmocom/gprs/gprs_msgb.h>

#include <osmocom/sigtran/sua.h>
#include <osmocom/sigtran/sccp_sap.h>

#include <openbsc/gprs_sgsn.h>
#include <openbsc/debug.h>

#include <osmocom/ranap/ranap_ies_defs.h>
#include <osmocom/ranap/ranap_common_cn.h>

int asn1_xer_print = 1;
void *talloc_asn1_ctx;

struct ue_conn_ctx {
	struct llist_head list;
	struct osmo_sua_link *link;
	uint32_t conn_id;
};

static LLIST_HEAD(conn_ctx_list);

struct ue_conn_ctx *ue_conn_ctx_alloc(struct osmo_sua_link *link, uint32_t conn_id)
{
	struct ue_conn_ctx *ctx = talloc_zero(NULL, struct ue_conn_ctx);

	ctx->link = link;
	ctx->conn_id = conn_id;
	llist_add(&ctx->list, &conn_ctx_list);

	return ctx;
}

struct ue_conn_ctx *ue_conn_ctx_find(struct osmo_sua_link *link, uint32_t conn_id)
{
	struct ue_conn_ctx *ctx;

	llist_for_each_entry(ctx, &conn_ctx_list, list) {
		if (ctx->link == link && ctx->conn_id == conn_id)
			return ctx;
	}
	return NULL;
}

/***********************************************************************
 * RANAP handling
 ***********************************************************************/

int gprs_iu_rab_act(struct sgsn_mm_ctx *mm, uint32_t gtp_ip, uint32_t gtp_tei)
{
	struct ue_conn_ctx *uectx;
	struct osmo_scu_prim *prim;
	struct msgb *msg;

	uectx = mm->iu.ue_ctx;

	msg = ranap_new_msg_rab_assign_data(1, gtp_ip, gtp_tei);
	msg->l2h = msg->data;

	/* wrap RANAP message in SCCP N-DATA.req */
	prim = (struct osmo_scu_prim *) msgb_push(msg, sizeof(*prim));
	prim->u.data.conn_id = uectx->conn_id;
	osmo_prim_init(&prim->oph, SCCP_SAP_USER,
			OSMO_SCU_PRIM_N_DATA,
			PRIM_OP_REQUEST, msg);
	osmo_sua_user_link_down(uectx->link, &prim->oph);
}

int gprs_iu_rab_deact(struct sgsn_mm_ctx *mm)
{

}

int gprs_transp_upd_key(struct sgsn_mm_ctx *mm)
{
	struct gsm_auth_tuple *tp = &mm->auth_triplet;

	if (mm->ran_type == MM_CTX_T_UTRAN_Iu) {
		struct ue_conn_ctx *uectx;
		struct osmo_scu_prim *prim;
		struct msgb *msg;
		uint8_t ik[16];
		uint8_t ck[16];
		unsigned int i;

		uectx = mm->iu.ue_ctx;

		/* C4 function to dervie CK from Kc */
		memcpy(ck, tp->kc, 8);
		memcpy(ck+8, tp->kc, 8);

		/* C5 function to derive IK from Kc */
		for (i = 0; i < 4; i++)
			ik[i] = tp->kc[i] ^ tp->kc[i+4];
		memcpy(ik+4, tp->kc, 8);
		for (i = 12; i < 16; i++)
			ik[i] = ik[i-12];

		/* crate RANAP message */
		msg = ranap_new_msg_sec_mod_cmd(ik, NULL);
		msg->l2h = msg->data;
		/* wrap RANAP message in SCCP N-DATA.req */
		prim = (struct osmo_scu_prim *) msgb_push(msg, sizeof(*prim));
		prim->u.data.conn_id = uectx->conn_id;
		osmo_prim_init(&prim->oph, SCCP_SAP_USER,
				OSMO_SCU_PRIM_N_DATA,
				PRIM_OP_REQUEST, msg);
		osmo_sua_user_link_down(uectx->link, &prim->oph);
	}

	return 0;
}

static int ranap_handle_co_initial_ue(void *ctx, RANAP_InitialUE_MessageIEs_t *ies)
{
	struct gprs_ra_id ra_id;
	uint16_t sai;
	struct msgb *msg = msgb_alloc(256, "RANAP->NAS");

	ranap_parse_lai(&ra_id, &ies->lai);
	sai = asn1str_to_u16(&ies->sai.sAC);
	msgb_gmmh(msg) = msgb_put(msg, ies->nas_pdu.size);
	memcpy(msgb_gmmh(msg), ies->nas_pdu.buf, ies->nas_pdu.size);

	/* Feed into the MM layer */
	msg->dst = ctx;
	gsm0408_gprs_rcvmsg_iu(msg, ra_id, sai);

	return 0;
}

static int ranap_handle_co_dt(void *ctx, RANAP_DirectTransferIEs_t *ies)
{
	struct gprs_ra_id _ra_id, *ra_id = NULL;
	uint16_t _sai, *sai = NULL;
	struct msgb *msg = msgb_alloc(256, "RANAP->NAS");

	if (ies->presenceMask & DIRECTTRANSFERIES_RANAP_LAI_PRESENT) {
		ranap_parse_lai(&_ra_id, &ies->lai);
		ra_id = &_ra_id;
		if (ies->presenceMask & DIRECTTRANSFERIES_RANAP_RAC_PRESENT) {
			_ra_id.rac = asn1str_to_u8(&ies->rac);
		}
		if (ies->presenceMask & DIRECTTRANSFERIES_RANAP_SAI_PRESENT) {
			_sai = asn1str_to_u16(&ies->sai.sAC);
			sai = &_sai;
		}
	}

	msgb_gmmh(msg) = msgb_put(msg, ies->nas_pdu.size);
	memcpy(msgb_gmmh(msg), ies->nas_pdu.buf, ies->nas_pdu.size);

	/* Feed into the MM/CC/SMS-CP layer */
	msg->dst = ctx;
	gsm0408_gprs_rcvmsg_iu(msg, ra_id, sai);

	return 0;
}

static int ranap_handle_co_err_ind(void *ctx, RANAP_ErrorIndicationIEs_t *ies)
{
	if (ies->presenceMask & ERRORINDICATIONIES_RANAP_CAUSE_PRESENT)
		LOGP(DRANAP, LOGL_ERROR, "Rx Error Indication (%s)\n",
			ranap_cause_str(&ies->cause));
	else
		LOGP(DRANAP, LOGL_ERROR, "Rx Error Indication\n");

	return 0;
}

int gprs_iu_tx(struct msgb *msg, uint8_t sapi, struct mm_context *mm)
{
	struct ue_conn_ctx *uectx = msg->dst;
	struct osmo_scu_prim *prim;

	LOGP(DRANAP, LOGL_INFO, "Transmitting L3 Message as RANAP DT\n");

	msg = ranap_new_msg_dt(sapi, msg->data, msgb_length(msg));
	msg->l2h = msg->data;
	prim = (struct osmo_scu_prim *) msgb_push(msg, sizeof(*prim));
	prim->u.data.conn_id = uectx->conn_id;
	osmo_prim_init(&prim->oph, SCCP_SAP_USER,
			OSMO_SCU_PRIM_N_DATA,
			PRIM_OP_REQUEST, msg);
	osmo_sua_user_link_down(uectx->link, &prim->oph);
	return 0;
}

static int ranap_handle_co_iu_rel_req(struct ue_conn_ctx *ctx, RANAP_Iu_ReleaseRequestIEs_t *ies)
{
	struct msgb *msg;
	struct osmo_scu_prim *prim;

	LOGP(DRANAP, LOGL_INFO, "Received Iu Release Request, Sending Release Command\n");
	msg = ranap_new_msg_iu_rel_cmd(&ies->cause);
	msg->l2h = msg->data;
	prim = (struct osmo_scu_prim *) msgb_push(msg, sizeof(*prim));
	prim->u.data.conn_id = ctx->conn_id;
	osmo_prim_init(&prim->oph, SCCP_SAP_USER,
			OSMO_SCU_PRIM_N_DATA,
			PRIM_OP_REQUEST, msg);
	osmo_sua_user_link_down(ctx->link, &prim->oph);
	return 0;
}

static int ranap_handle_co_rab_ass_resp(void *ctx, RANAP_RAB_AssignmentResponseIEs_t *ies)
{
	int i, rc;

	LOGP(DRANAP, LOGL_INFO, "RAB Asignment Response:");
	if (ies->presenceMask & RAB_ASSIGNMENTRESPONSEIES_RANAP_RAB_SETUPORMODIFIEDLIST_PRESENT) {
		RANAP_RAB_SetupOrModifiedItemIEs_t setup_ies;
		RANAP_RAB_SetupOrModifiedItem_t *item = &setup_ies.raB_SetupOrModifiedItem;
		rc = ranap_decode_rab_setupormodifieditemies(&setup_ies, &ies->raB_SetupOrModifiedList);
		if (item->transportLayerAddress) {
			uint8_t rab_id = item->rAB_ID.buf[0];
			LOGPC(DRANAP, LOGL_INFO, " Setup: (%u/%s)", osmo_hexdump(item->transportLayerAddress->buf,
									     item->transportLayerAddress->size));
		}
	}

	LOGPC(DRANAP, LOGL_INFO, "\n");

	return rc;
}

/* Entry point for connection-oriented ANAP message */
static void cn_ranap_handle_co(void *ctx, ranap_message *message)
{
	int rc = 0;

	LOGP(DRANAP, LOGL_NOTICE, "handle_co(dir=%u, proc=%u)\n", message->direction, message->procedureCode);

	switch (message->direction) {
	case RANAP_RANAP_PDU_PR_initiatingMessage:
		switch (message->procedureCode) {
		case RANAP_ProcedureCode_id_InitialUE_Message:
			rc = ranap_handle_co_initial_ue(ctx, &message->msg.initialUE_MessageIEs);
			break;
		case RANAP_ProcedureCode_id_DirectTransfer:
			rc = ranap_handle_co_dt(ctx, &message->msg.directTransferIEs);
			break;
		case RANAP_ProcedureCode_id_ErrorIndication:
			rc = ranap_handle_co_err_ind(ctx, &message->msg.errorIndicationIEs);
			break;
		case RANAP_ProcedureCode_id_Iu_ReleaseRequest:
			/* Iu Release Request */
			rc = ranap_handle_co_iu_rel_req(ctx, &message->msg.iu_ReleaseRequestIEs);
			break;
		}
		break;
	case RANAP_RANAP_PDU_PR_successfulOutcome:
		switch (message->procedureCode) {
		case RANAP_ProcedureCode_id_RAB_Assignment:
			/* RAB Assignment Response */
			rc = ranap_handle_co_rab_ass_resp(ctx, &message->msg.raB_AssignmentResponseIEs);
			break;
		case RANAP_ProcedureCode_id_SecurityModeControl:
			/* Security Mode Complete */
			break;
		case RANAP_ProcedureCode_id_Iu_Release:
			/* Iu Release Complete */
			break;
		}
	case RANAP_RANAP_PDU_PR_unsuccessfulOutcome:
	case RANAP_RANAP_PDU_PR_outcome:
	default:
		rc = -1;
		break;
	}
}

static int ranap_handle_cl_reset_req(void *ctx, RANAP_ResetIEs_t *ies)
{
	/* FIXME: send reset response */
}

static int ranap_handle_cl_err_ind(void *ctx, RANAP_ErrorIndicationIEs_t *ies)
{
	if (ies->presenceMask & ERRORINDICATIONIES_RANAP_CAUSE_PRESENT)
		LOGP(DRANAP, LOGL_ERROR, "Rx Error Indication (%s)\n",
			ranap_cause_str(&ies->cause));
	else
		LOGP(DRANAP, LOGL_ERROR, "Rx Error Indication\n");

	return 0;
}

/* Entry point for connection-less RANAP message */
static void cn_ranap_handle_cl(void *ctx, ranap_message *message)
{
	int rc = 0;

	switch (message->direction) {
	case RANAP_RANAP_PDU_PR_initiatingMessage:
		switch (message->procedureCode) {
		case RANAP_ProcedureCode_id_Reset:
			/* received reset.req, send reset.resp */
			rc = ranap_handle_cl_reset_req(ctx, &message->msg.resetIEs);
			break;
		case RANAP_ProcedureCode_id_ErrorIndication:
			rc = ranap_handle_cl_err_ind(ctx, &message->msg.errorIndicationIEs);
			break;
		}
		break;
	case RANAP_RANAP_PDU_PR_successfulOutcome:
	case RANAP_RANAP_PDU_PR_unsuccessfulOutcome:
	case RANAP_RANAP_PDU_PR_outcome:
	default:
		rc = -1;
		break;
	}
}

/***********************************************************************
 *
 ***********************************************************************/

int tx_unitdata(struct osmo_sua_link *link);
int tx_conn_req(struct osmo_sua_link *link, uint32_t conn_id);

struct osmo_prim_hdr *make_conn_req(uint32_t conn_id);
struct osmo_prim_hdr *make_dt1_req(uint32_t conn_id, const uint8_t *data, unsigned int len);

struct osmo_prim_hdr *make_conn_resp(struct osmo_scu_connect_param *param)
{
	struct msgb *msg = msgb_alloc(1024, "conn_resp");
	struct osmo_scu_prim *prim;

	prim = (struct osmo_scu_prim *) msgb_put(msg, sizeof(*prim));
	osmo_prim_init(&prim->oph, SCCP_SAP_USER,
			OSMO_SCU_PRIM_N_CONNECT,
			PRIM_OP_RESPONSE, msg);
	memcpy(&prim->u.connect, param, sizeof(prim->u.connect));
	return &prim->oph;
}

static int sccp_sap_up(struct osmo_prim_hdr *oph, void *link)
{
	struct osmo_scu_prim *prim = (struct osmo_scu_prim *) oph;
	struct osmo_prim_hdr *resp = NULL;
	const uint8_t payload[] = { 0xb1, 0xb2, 0xb3 };
	int rc;
	struct ue_conn_ctx *ue;

	printf("sccp_sap_up(%s)\n", osmo_scu_prim_name(oph));

	switch (OSMO_PRIM_HDR(oph)) {
	case OSMO_PRIM(OSMO_SCU_PRIM_N_CONNECT, PRIM_OP_CONFIRM):
		/* confirmation of outbound connection */
		break;
	case OSMO_PRIM(OSMO_SCU_PRIM_N_CONNECT, PRIM_OP_INDICATION):
		/* indication of new inbound connection request*/
		printf("N-CONNECT.ind(X->%u)\n", prim->u.connect.conn_id);
		if (/*  prim->u.connect.called_addr.ssn != OSMO_SCCP_SSN_RANAP || */
		    !msgb_l2(oph->msg) || msgb_l2len(oph->msg) == 0) {
			LOGP(DGPRS, LOGL_NOTICE, "Received invalid N-CONNECT.ind\n");
			return 0;
		}
		/* FIXME: allocate UE context */
		ue = ue_conn_ctx_alloc(link, prim->u.connect.conn_id);
		/* first ensure the local SUA/SCCP socket is ACTIVE */
		resp = make_conn_resp(&prim->u.connect);
		osmo_sua_user_link_down(link, resp);
		/* then handle the RANAP payload */
		rc = ranap_cn_rx_co(cn_ranap_handle_co, ue, msgb_l2(oph->msg), msgb_l2len(oph->msg));
		break;
	case OSMO_PRIM(OSMO_SCU_PRIM_N_DISCONNECT, PRIM_OP_INDICATION):
		/* indication of disconnect */
		printf("N-DISCONNECT.ind(%u)\n", prim->u.disconnect.conn_id);
		ue = ue_conn_ctx_find(link, prim->u.disconnect.conn_id);
		rc = ranap_cn_rx_co(cn_ranap_handle_co, ue, msgb_l2(oph->msg), msgb_l2len(oph->msg));
		break;
	case OSMO_PRIM(OSMO_SCU_PRIM_N_DATA, PRIM_OP_INDICATION):
		/* connection-oriented data received */
		printf("N-DATA.ind(%u, %s)\n", prim->u.data.conn_id,
			osmo_hexdump(msgb_l2(oph->msg), msgb_l2len(oph->msg)));
		/* resolve UE context */
		ue = ue_conn_ctx_find(link, prim->u.data.conn_id);
		rc = ranap_cn_rx_co(cn_ranap_handle_co, ue, msgb_l2(oph->msg), msgb_l2len(oph->msg));
		break;
	case OSMO_PRIM(OSMO_SCU_PRIM_N_UNITDATA, PRIM_OP_INDICATION):
		/* connection-oriented data received */
		printf("N-UNITDATA.ind(%s)\n", 
			osmo_hexdump(msgb_l2(oph->msg), msgb_l2len(oph->msg)));
		rc = ranap_cn_rx_cl(cn_ranap_handle_cl, link, msgb_l2(oph->msg), msgb_l2len(oph->msg));
		break;
	}

	msgb_free(oph->msg);
	return 0;
}

int sgsn_iu_init(void *ctx)
{
	struct osmo_sua_user *user;
	int rc;

	talloc_asn1_ctx = talloc_named_const(ctx, 1, "asn1");

	osmo_sua_set_log_area(DSUA);

	user = osmo_sua_user_create(ctx, sccp_sap_up, ctx);

	rc = osmo_sua_server_listen(user, "127.0.0.2", 14001);
	if (rc < 0) {
		exit(1);
	}
}
