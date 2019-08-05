/**
* Copyright (C) Mellanox Technologies Ltd. 2019.  ALL RIGHTS RESERVED.
*
* See file LICENSE for terms.
*/

#include "rdmacm_cm_ep.h"


static ucs_status_t uct_rdmacm_create_dummy_cq_qp(struct rdma_cm_id *id,
                                                  struct ibv_cq **cq_p,
                                                  struct ibv_qp **qp_p)
{
    struct ibv_qp_init_attr qp_init_attr;
    ucs_status_t status;
    struct ibv_cq *cq;
    struct ibv_qp *qp;

    /* Create a dummy completion queue */
    cq = ibv_create_cq(id->verbs, 1, NULL, NULL, 0);
    if (cq == NULL) {
        ucs_error("ibv_create_cq() failed: %m");
        status =  UCS_ERR_IO_ERROR;
        goto err;
    }

    /* Create a dummy UD qp */
    memset(&qp_init_attr, 0, sizeof(qp_init_attr));
    qp_init_attr.send_cq = cq;
    qp_init_attr.recv_cq = cq;
    qp_init_attr.qp_type = IBV_QPT_UD;
    qp_init_attr.cap.max_send_wr  = 2;
    qp_init_attr.cap.max_recv_wr  = 2;
    qp_init_attr.cap.max_send_sge = 1;
    qp_init_attr.cap.max_recv_sge = 1;

    qp = ibv_create_qp(id->pd, &qp_init_attr);
    if (qp == NULL) {
        ucs_error("failed to create a dummy ud qp. %m");
        status = UCS_ERR_IO_ERROR;
        goto err_destroy_cq;
    }

    ucs_debug("created ud QP %p with qp_num: 0x%x and cq %p on rdmacm_id %p",
              qp, qp->qp_num, cq, id);

    *cq_p = cq;
    *qp_p = qp;

    return UCS_OK;

err_destroy_cq:
    ibv_destroy_cq(cq);
err:
    return status;
}

ucs_status_t
uct_rdamcm_cm_ep_set_qp_num(struct rdma_conn_param *conn_param,
                            const uct_rdmacm_priv_data_hdr_t *hdr,
                            uct_rdmacm_cm_ep_t *cep)
{
    ucs_status_t status;
    struct ibv_qp *qp;
    struct ibv_cq *cq;

    /* create a dummy qp in order to get a unique qp_num to provide to librdmacm */
    status = uct_rdmacm_create_dummy_cq_qp(cep->id, &cq, &qp);
    if (status != UCS_OK) {
        return status;
    }

    cep->cq             = cq;
    cep->qp             = qp;
    conn_param->qp_num  = qp->qp_num;
    return UCS_OK;
}

static ucs_status_t uct_rdamcm_cm_ep_client_init(uct_rdmacm_cm_ep_t *cep,
                                                 const uct_ep_params_t *params)
{
    char ip_port_str[UCS_SOCKADDR_STRING_LEN];
    ucs_status_t status;

    cep->wireup.client.connect_cb = params->sockaddr_connect_cb.client;

    if (rdma_create_id(cep->cm->ev_ch, &cep->id, cep, RDMA_PS_TCP)) {
        ucs_error("rdma_create_id() failed: %m");
        status = UCS_ERR_IO_ERROR;
        goto err;
    }

    /* rdma_resolve_addr needs to be called last in the ep_create flow to
     * prevent a race where there are uninitialized fields used when the
     * RDMA_CM_EVENT_ROUTE_RESOLVED event is already received in the the async
     * thread. Therefore, all ep fields have to be initialized before this
     * function is called. */
    if (rdma_resolve_addr(cep->id, NULL, (struct sockaddr *)params->sockaddr->addr,
                          1000/* TODO */)) {
        ucs_error("rdma_resolve_addr() to dst addr %s failed: %m",
                  ucs_sockaddr_str((struct sockaddr *)params->sockaddr->addr,
                                   ip_port_str, UCS_SOCKADDR_STRING_LEN));
        status = UCS_ERR_IO_ERROR;
        goto err_destroy_id;
    }

    return UCS_OK;

err_destroy_id:
    uct_rdmacm_cm_destroy_id(cep->id);
err:
    return status;
}

static ucs_status_t uct_rdamcm_cm_ep_server_init(uct_rdmacm_cm_ep_t *cep,
                                                 const uct_ep_params_t *params)
{
    return UCS_ERR_NOT_IMPLEMENTED;
}

ucs_status_t uct_rdmacm_cm_ep_disconnect(uct_ep_h ep, unsigned flags)
{
    uct_rdmacm_cm_ep_t *cep = ucs_derived_of(ep, uct_rdmacm_cm_ep_t);
    char ip_port_str[UCS_SOCKADDR_STRING_LEN];

    if (rdma_disconnect(cep->id)) {
        ucs_error("rdmacm_cm ep %p (id=%p) failed to disconnect from peer %p",
                  cep, cep->id,
                  ucs_sockaddr_str(rdma_get_peer_addr(cep->id), ip_port_str,
                                   UCS_SOCKADDR_STRING_LEN));
        return UCS_ERR_IO_ERROR;
    }

    ucs_debug("rdmacm_cm ep %p (id=%p) disconnecting from peer :%s", cep, cep->id,
              ucs_sockaddr_str(rdma_get_peer_addr(cep->id), ip_port_str,
                               UCS_SOCKADDR_STRING_LEN));
    return UCS_OK;
}

UCS_CLASS_INIT_FUNC(uct_rdmacm_cm_ep_t, const uct_ep_params_t *params)
{
    ucs_status_t status;
    uct_rdmacm_cm_t *cm;

    if (!(params->field_mask & UCT_EP_PARAM_FIELD_CM)) {
        ucs_error("UCT_EP_PARAM_FIELD_CM is not set. field_mask 0x%lx",
                  params->field_mask);
        return UCS_ERR_INVALID_PARAM;
    }

    if (!(params->field_mask & UCT_EP_PARAM_FIELD_SOCKADDR_CB_FLAGS) ||
        !(params->sockaddr_cb_flags & UCT_CB_FLAG_ASYNC)) {
        ucs_error("UCT_EP_PARAM_FIELD_SOCKADDR_CB_FLAGS and UCT_CB_FLAG_ASYNC "
                  "should be set");
        return UCS_ERR_UNSUPPORTED;
    }

    if (!(params->field_mask & (UCT_EP_PARAM_FIELD_SOCKADDR |
                                UCT_EP_PARAM_FIELD_CONN_REQUEST))) {
        ucs_error("neither UCT_EP_PARAM_FIELD_SOCKADDR nor "
                  "UCT_EP_PARAM_FIELD_CONN_REQUEST is set. field_mask 0x%lx",
                  params->field_mask);
        return UCS_ERR_INVALID_PARAM;
    }

    cm = ucs_derived_of(params->cm, uct_rdmacm_cm_t);
    UCS_CLASS_CALL_SUPER_INIT(uct_base_ep_t, &cm->super.iface);

    self->cm                  = cm;
    self->wireup.priv_pack_cb = (params->field_mask &
                                 UCT_EP_PARAM_FIELD_SOCKADDR_PACK_CB) ?
                                params->sockaddr_pack_cb : NULL;
    self->disconnect_cb       = (params->field_mask &
                                 UCT_EP_PARAM_FIELD_SOCKADDR_DISCONNECT_CB) ?
                                params->disconnect_cb : NULL;
    self->user_data           = (params->field_mask &
                                 UCT_EP_PARAM_FIELD_USER_DATA) ?
                                params->user_data : NULL;
    self->cq                  = NULL;
    self->qp                  = NULL;

    if (params->field_mask & UCT_EP_PARAM_FIELD_SOCKADDR) {
        status = uct_rdamcm_cm_ep_client_init(self, params);
    } else if (params->field_mask & UCT_EP_PARAM_FIELD_CONN_REQUEST) {
        status = uct_rdamcm_cm_ep_server_init(self, params);
    } else {
        ucs_error("either UCT_EP_PARAM_FIELD_SOCKADDR or UCT_EP_PARAM_FIELD_CONN_REQUEST "
                  "has to be provided");
        status = UCS_ERR_INVALID_PARAM;
    }

    if (status == UCS_OK) {
        ucs_debug("created an endpoint %p on rdmacm %p id: %p",
                  self, self->cm, self->id);
    }

    return status;
}

UCS_CLASS_CLEANUP_FUNC(uct_rdmacm_cm_ep_t)
{
    uct_priv_worker_t *worker_priv = ucs_derived_of(self->cm->super.iface.worker,
                                                    uct_priv_worker_t);
    int ret;

    UCS_ASYNC_BLOCK(worker_priv->async);

    if (self->qp != NULL) {
        ret = ibv_destroy_qp(self->qp);
        if (ret != 0) {
            ucs_warn("ibv_destroy_qp() returned %d: %m", ret);
        }
    }

    if (self->cq != NULL) {
        ret = ibv_destroy_cq(self->cq);
        if (ret != 0) {
            ucs_warn("ibv_destroy_cq() returned %d: %m", ret);
        }
    }

    /* rdma_destroy_id() cleans all events not yet reported on progress thread,
     * so no events would be reported to the user after destroying the id */
    uct_rdmacm_cm_destroy_id(self->id);

    UCS_ASYNC_UNBLOCK(worker_priv->async);
}

UCS_CLASS_DEFINE(uct_rdmacm_cm_ep_t, uct_base_ep_t);
UCS_CLASS_DEFINE_NEW_FUNC(uct_rdmacm_cm_ep_t, uct_ep_t, const uct_ep_params_t *);
UCS_CLASS_DEFINE_DELETE_FUNC(uct_rdmacm_cm_ep_t, uct_ep_t);