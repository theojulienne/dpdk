/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2022 NVIDIA Corporation & Affiliates
 */

#include "mlx5dr_internal.h"

static uint32_t mlx5dr_cmd_get_syndrome(uint32_t *out)
{
	/* Assumption: syndrome is always the second u32 */
	return be32toh(out[1]);
}

int mlx5dr_cmd_destroy_obj(struct mlx5dr_devx_obj *devx_obj)
{
	int ret;

	ret = mlx5_glue->devx_obj_destroy(devx_obj->obj);
	simple_free(devx_obj);

	return ret;
}

struct mlx5dr_devx_obj *
mlx5dr_cmd_flow_table_create(struct ibv_context *ctx,
			     struct mlx5dr_cmd_ft_create_attr *ft_attr)
{
	uint32_t out[MLX5_ST_SZ_DW(create_flow_table_out)] = {0};
	uint32_t in[MLX5_ST_SZ_DW(create_flow_table_in)] = {0};
	struct mlx5dr_devx_obj *devx_obj;
	void *ft_ctx;

	devx_obj = simple_malloc(sizeof(*devx_obj));
	if (!devx_obj) {
		DR_LOG(ERR, "Failed to allocate memory for flow table object");
		rte_errno = ENOMEM;
		return NULL;
	}

	MLX5_SET(create_flow_table_in, in, opcode, MLX5_CMD_OP_CREATE_FLOW_TABLE);
	MLX5_SET(create_flow_table_in, in, table_type, ft_attr->type);

	ft_ctx = MLX5_ADDR_OF(create_flow_table_in, in, flow_table_context);
	MLX5_SET(flow_table_context, ft_ctx, level, ft_attr->level);
	MLX5_SET(flow_table_context, ft_ctx, rtc_valid, ft_attr->rtc_valid);
	MLX5_SET(flow_table_context, ft_ctx, reformat_en, ft_attr->reformat_en);

	devx_obj->obj = mlx5_glue->devx_obj_create(ctx, in, sizeof(in), out, sizeof(out));
	if (!devx_obj->obj) {
		DR_LOG(ERR, "Failed to create FT (syndrome: %#x)",
		       mlx5dr_cmd_get_syndrome(out));
		simple_free(devx_obj);
		rte_errno = errno;
		return NULL;
	}

	devx_obj->id = MLX5_GET(create_flow_table_out, out, table_id);

	return devx_obj;
}

int
mlx5dr_cmd_flow_table_modify(struct mlx5dr_devx_obj *devx_obj,
			     struct mlx5dr_cmd_ft_modify_attr *ft_attr)
{
	uint32_t out[MLX5_ST_SZ_DW(modify_flow_table_out)] = {0};
	uint32_t in[MLX5_ST_SZ_DW(modify_flow_table_in)] = {0};
	void *ft_ctx;
	int ret;

	MLX5_SET(modify_flow_table_in, in, opcode, MLX5_CMD_OP_MODIFY_FLOW_TABLE);
	MLX5_SET(modify_flow_table_in, in, table_type, ft_attr->type);
	MLX5_SET(modify_flow_table_in, in, modify_field_select, ft_attr->modify_fs);
	MLX5_SET(modify_flow_table_in, in, table_id, devx_obj->id);

	ft_ctx = MLX5_ADDR_OF(modify_flow_table_in, in, flow_table_context);

	MLX5_SET(flow_table_context, ft_ctx, table_miss_action, ft_attr->table_miss_action);
	MLX5_SET(flow_table_context, ft_ctx, table_miss_id, ft_attr->table_miss_id);
	MLX5_SET(flow_table_context, ft_ctx, rtc_id_0, ft_attr->rtc_id_0);
	MLX5_SET(flow_table_context, ft_ctx, rtc_id_1, ft_attr->rtc_id_1);

	ret = mlx5_glue->devx_obj_modify(devx_obj->obj, in, sizeof(in), out, sizeof(out));
	if (ret) {
		DR_LOG(ERR, "Failed to modify FT (syndrome: %#x)",
		       mlx5dr_cmd_get_syndrome(out));
		rte_errno = errno;
	}

	return ret;
}

int
mlx5dr_cmd_flow_table_query(struct mlx5dr_devx_obj *devx_obj,
			    struct mlx5dr_cmd_ft_query_attr *ft_attr,
			    uint64_t *icm_addr_0, uint64_t *icm_addr_1)
{
	uint32_t out[MLX5_ST_SZ_DW(query_flow_table_out)] = {0};
	uint32_t in[MLX5_ST_SZ_DW(query_flow_table_in)] = {0};
	void *ft_ctx;
	int ret;

	MLX5_SET(query_flow_table_in, in, opcode, MLX5_CMD_OP_QUERY_FLOW_TABLE);
	MLX5_SET(query_flow_table_in, in, table_type, ft_attr->type);
	MLX5_SET(query_flow_table_in, in, table_id, devx_obj->id);

	ret = mlx5_glue->devx_obj_query(devx_obj->obj, in, sizeof(in), out, sizeof(out));
	if (ret) {
		DR_LOG(ERR, "Failed to query FT (syndrome: %#x)",
		       mlx5dr_cmd_get_syndrome(out));
		rte_errno = errno;
		return ret;
	}

	ft_ctx = MLX5_ADDR_OF(query_flow_table_out, out, flow_table_context);
	*icm_addr_0 = MLX5_GET64(flow_table_context, ft_ctx, sw_owner_icm_root_0);
	*icm_addr_1 = MLX5_GET64(flow_table_context, ft_ctx, sw_owner_icm_root_1);

	return ret;
}

static struct mlx5dr_devx_obj *
mlx5dr_cmd_flow_group_create(struct ibv_context *ctx,
			     struct mlx5dr_cmd_fg_attr *fg_attr)
{
	uint32_t out[MLX5_ST_SZ_DW(create_flow_group_out)] = {0};
	uint32_t in[MLX5_ST_SZ_DW(create_flow_group_in)] = {0};
	struct mlx5dr_devx_obj *devx_obj;

	devx_obj = simple_malloc(sizeof(*devx_obj));
	if (!devx_obj) {
		DR_LOG(ERR, "Failed to allocate memory for flow group object");
		rte_errno = ENOMEM;
		return NULL;
	}

	MLX5_SET(create_flow_group_in, in, opcode, MLX5_CMD_OP_CREATE_FLOW_GROUP);
	MLX5_SET(create_flow_group_in, in, table_type, fg_attr->table_type);
	MLX5_SET(create_flow_group_in, in, table_id, fg_attr->table_id);

	devx_obj->obj = mlx5_glue->devx_obj_create(ctx, in, sizeof(in), out, sizeof(out));
	if (!devx_obj->obj) {
		DR_LOG(ERR, "Failed to create Flow group(syndrome: %#x)",
		       mlx5dr_cmd_get_syndrome(out));
		simple_free(devx_obj);
		rte_errno = errno;
		return NULL;
	}

	devx_obj->id = MLX5_GET(create_flow_group_out, out, group_id);

	return devx_obj;
}

struct mlx5dr_devx_obj *
mlx5dr_cmd_set_fte(struct ibv_context *ctx,
		   uint32_t table_type,
		   uint32_t table_id,
		   uint32_t group_id,
		   struct mlx5dr_cmd_set_fte_attr *fte_attr)
{
	uint32_t out[MLX5_ST_SZ_DW(set_fte_out)] = {0};
	struct mlx5dr_devx_obj *devx_obj;
	uint32_t dest_entry_sz;
	uint32_t total_dest_sz;
	void *in_flow_context;
	uint32_t action_flags;
	uint8_t *in_dests;
	uint32_t inlen;
	uint32_t *in;
	uint32_t i;

	dest_entry_sz = fte_attr->extended_dest ?
			MLX5_ST_SZ_BYTES(extended_dest_format) :
			MLX5_ST_SZ_BYTES(dest_format);
	total_dest_sz = dest_entry_sz * fte_attr->dests_num;
	inlen = align((MLX5_ST_SZ_BYTES(set_fte_in) + total_dest_sz), DW_SIZE);
	in = simple_calloc(1, inlen);
	if (!in) {
		rte_errno = ENOMEM;
		return NULL;
	}

	devx_obj = simple_malloc(sizeof(*devx_obj));
	if (!devx_obj) {
		DR_LOG(ERR, "Failed to allocate memory for fte object");
		rte_errno = ENOMEM;
		goto free_in;
	}

	MLX5_SET(set_fte_in, in, opcode, MLX5_CMD_OP_SET_FLOW_TABLE_ENTRY);
	MLX5_SET(set_fte_in, in, table_type, table_type);
	MLX5_SET(set_fte_in, in, table_id, table_id);

	in_flow_context = MLX5_ADDR_OF(set_fte_in, in, flow_context);
	MLX5_SET(flow_context, in_flow_context, group_id, group_id);
	MLX5_SET(flow_context, in_flow_context, flow_source, fte_attr->flow_source);
	MLX5_SET(flow_context, in_flow_context, extended_destination, fte_attr->extended_dest);
	MLX5_SET(set_fte_in, in, ignore_flow_level, fte_attr->ignore_flow_level);

	action_flags = fte_attr->action_flags;
	MLX5_SET(flow_context, in_flow_context, action, action_flags);

	if (action_flags & MLX5_FLOW_CONTEXT_ACTION_REFORMAT)
		MLX5_SET(flow_context, in_flow_context,
			 packet_reformat_id, fte_attr->packet_reformat_id);

	if (action_flags & (MLX5_FLOW_CONTEXT_ACTION_DECRYPT | MLX5_FLOW_CONTEXT_ACTION_ENCRYPT)) {
		MLX5_SET(flow_context, in_flow_context,
			 encrypt_decrypt_type, fte_attr->encrypt_decrypt_type);
		MLX5_SET(flow_context, in_flow_context,
			 encrypt_decrypt_obj_id, fte_attr->encrypt_decrypt_obj_id);
	}

	if (action_flags & MLX5_FLOW_CONTEXT_ACTION_FWD_DEST) {
		in_dests = (uint8_t *)MLX5_ADDR_OF(flow_context, in_flow_context, destination);

		for (i = 0; i < fte_attr->dests_num; i++) {
			struct mlx5dr_cmd_set_fte_dest *dest = &fte_attr->dests[i];

			switch (dest->destination_type) {
			case MLX5_FLOW_DESTINATION_TYPE_VPORT:
				if (dest->ext_flags & MLX5DR_CMD_EXT_DEST_ESW_OWNER_VHCA_ID) {
					MLX5_SET(dest_format, in_dests,
						 destination_eswitch_owner_vhca_id_valid, 1);
					MLX5_SET(dest_format, in_dests,
						 destination_eswitch_owner_vhca_id,
						 dest->esw_owner_vhca_id);
				}
				/* Fall through */
			case MLX5_FLOW_DESTINATION_TYPE_TIR:
			case MLX5_FLOW_DESTINATION_TYPE_FLOW_TABLE:
				MLX5_SET(dest_format, in_dests, destination_type,
					 dest->destination_type);
				MLX5_SET(dest_format, in_dests, destination_id,
					 dest->destination_id);
				if (dest->ext_flags & MLX5DR_CMD_EXT_DEST_REFORMAT) {
					MLX5_SET(dest_format, in_dests, packet_reformat, 1);
					MLX5_SET(extended_dest_format, in_dests, packet_reformat_id,
						 dest->ext_reformat->id);
				}
				break;
			case MLX5_FLOW_DESTINATION_TYPE_NOP:
				MLX5_SET(dest_format, in_dests, destination_type,
					 dest->destination_type);
				break;
			default:
				rte_errno = EOPNOTSUPP;
				goto free_devx;
			}

			in_dests = in_dests + dest_entry_sz;
		}
		MLX5_SET(flow_context, in_flow_context, destination_list_size, fte_attr->dests_num);
	}

	devx_obj->obj = mlx5_glue->devx_obj_create(ctx, in, inlen, out, sizeof(out));
	if (!devx_obj->obj) {
		DR_LOG(ERR, "Failed to create FTE (syndrome: %#x)",
		       mlx5dr_cmd_get_syndrome(out));
		rte_errno = errno;
		goto free_devx;
	}

	simple_free(in);
	return devx_obj;

free_devx:
	simple_free(devx_obj);
free_in:
	simple_free(in);
	return NULL;
}

struct mlx5dr_cmd_forward_tbl *
mlx5dr_cmd_forward_tbl_create(struct ibv_context *ctx,
			      struct mlx5dr_cmd_ft_create_attr *ft_attr,
			      struct mlx5dr_cmd_set_fte_attr *fte_attr)
{
	struct mlx5dr_cmd_fg_attr fg_attr = {0};
	struct mlx5dr_cmd_forward_tbl *tbl;

	tbl = simple_calloc(1, sizeof(*tbl));
	if (!tbl) {
		DR_LOG(ERR, "Failed to allocate memory");
		rte_errno = ENOMEM;
		return NULL;
	}

	tbl->ft = mlx5dr_cmd_flow_table_create(ctx, ft_attr);
	if (!tbl->ft) {
		DR_LOG(ERR, "Failed to create FT");
		goto free_tbl;
	}

	fg_attr.table_id = tbl->ft->id;
	fg_attr.table_type = ft_attr->type;

	tbl->fg = mlx5dr_cmd_flow_group_create(ctx, &fg_attr);
	if (!tbl->fg) {
		DR_LOG(ERR, "Failed to create FG");
		goto free_ft;
	}

	tbl->fte = mlx5dr_cmd_set_fte(ctx, ft_attr->type, tbl->ft->id, tbl->fg->id, fte_attr);
	if (!tbl->fte) {
		DR_LOG(ERR, "Failed to create FTE");
		goto free_fg;
	}
	return tbl;

free_fg:
	mlx5dr_cmd_destroy_obj(tbl->fg);
free_ft:
	mlx5dr_cmd_destroy_obj(tbl->ft);
free_tbl:
	simple_free(tbl);
	return NULL;
}

void mlx5dr_cmd_forward_tbl_destroy(struct mlx5dr_cmd_forward_tbl *tbl)
{
	mlx5dr_cmd_destroy_obj(tbl->fte);
	mlx5dr_cmd_destroy_obj(tbl->fg);
	mlx5dr_cmd_destroy_obj(tbl->ft);
	simple_free(tbl);
}

void mlx5dr_cmd_set_attr_connect_miss_tbl(struct mlx5dr_context *ctx,
					  uint32_t fw_ft_type,
					  enum mlx5dr_table_type type,
					  struct mlx5dr_cmd_ft_modify_attr *ft_attr)
{
	struct mlx5dr_devx_obj *default_miss_tbl;

	if (!mlx5dr_table_is_fdb_any(type) && !mlx5dr_context_shared_gvmi_used(ctx))
		return;

	ft_attr->modify_fs = MLX5_IFC_MODIFY_FLOW_TABLE_MISS_ACTION;
	ft_attr->type = fw_ft_type;
	ft_attr->table_miss_action = MLX5_IFC_MODIFY_FLOW_TABLE_MISS_ACTION_GOTO_TBL;

	if (mlx5dr_table_is_fdb_any(type)) {
		default_miss_tbl = ctx->common_res[type].default_miss->ft;
		if (!default_miss_tbl) {
			assert(false);
			return;
		}
		ft_attr->table_miss_id = default_miss_tbl->id;
	} else {
		ft_attr->table_miss_id = ctx->gvmi_res[type].aliased_end_ft->id;
	}
}

struct mlx5dr_devx_obj *
mlx5dr_cmd_rtc_create(struct ibv_context *ctx,
		      struct mlx5dr_cmd_rtc_create_attr *rtc_attr)
{
	uint32_t out[MLX5_ST_SZ_DW(general_obj_out_cmd_hdr)] = {0};
	uint32_t in[MLX5_ST_SZ_DW(create_rtc_in)] = {0};
	struct mlx5dr_devx_obj *devx_obj;
	void *attr;

	devx_obj = simple_malloc(sizeof(*devx_obj));
	if (!devx_obj) {
		DR_LOG(ERR, "Failed to allocate memory for RTC object");
		rte_errno = ENOMEM;
		return NULL;
	}

	attr = MLX5_ADDR_OF(create_rtc_in, in, hdr);
	MLX5_SET(general_obj_in_cmd_hdr,
		 attr, opcode, MLX5_CMD_OP_CREATE_GENERAL_OBJECT);
	MLX5_SET(general_obj_in_cmd_hdr,
		 attr, obj_type, MLX5_GENERAL_OBJ_TYPE_RTC);

	attr = MLX5_ADDR_OF(create_rtc_in, in, rtc);
	if (rtc_attr->is_compare) {
		MLX5_SET(rtc, attr, ste_format_0, MLX5_IFC_RTC_STE_FORMAT_4DW_RANGE);
	} else {
		MLX5_SET(rtc, attr, ste_format_0, rtc_attr->is_frst_jumbo ?
			 MLX5_IFC_RTC_STE_FORMAT_11DW : MLX5_IFC_RTC_STE_FORMAT_8DW);
	}

	if (rtc_attr->is_scnd_range) {
		MLX5_SET(rtc, attr, ste_format_1, MLX5_IFC_RTC_STE_FORMAT_RANGE);
		MLX5_SET(rtc, attr, num_match_ste, 2);
	}

	MLX5_SET(rtc, attr, pd, rtc_attr->pd);
	MLX5_SET(rtc, attr, update_method, rtc_attr->fw_gen_wqe);
	MLX5_SET(rtc, attr, update_index_mode, rtc_attr->update_index_mode);
	MLX5_SET(rtc, attr, access_index_mode, rtc_attr->access_index_mode);
	MLX5_SET(rtc, attr, num_hash_definer, rtc_attr->num_hash_definer);
	MLX5_SET(rtc, attr, log_depth, rtc_attr->log_depth);
	MLX5_SET(rtc, attr, log_hash_size, rtc_attr->log_size);
	MLX5_SET(rtc, attr, table_type, rtc_attr->table_type);
	MLX5_SET(rtc, attr, num_hash_definer, rtc_attr->num_hash_definer);
	MLX5_SET(rtc, attr, match_definer_0, rtc_attr->match_definer_0);
	MLX5_SET(rtc, attr, match_definer_1, rtc_attr->match_definer_1);
	MLX5_SET(rtc, attr, stc_id, rtc_attr->stc_base);
	MLX5_SET(rtc, attr, ste_table_base_id, rtc_attr->ste_base);
	MLX5_SET(rtc, attr, ste_table_offset, rtc_attr->ste_offset);
	MLX5_SET(rtc, attr, miss_flow_table_id, rtc_attr->miss_ft_id);
	MLX5_SET(rtc, attr, reparse_mode, rtc_attr->reparse_mode);

	devx_obj->obj = mlx5_glue->devx_obj_create(ctx, in, sizeof(in), out, sizeof(out));
	if (!devx_obj->obj) {
		DR_LOG(ERR, "Failed to create RTC (syndrome: %#x)",
		       mlx5dr_cmd_get_syndrome(out));
		simple_free(devx_obj);
		rte_errno = errno;
		return NULL;
	}

	devx_obj->id = MLX5_GET(general_obj_out_cmd_hdr, out, obj_id);

	return devx_obj;
}

struct mlx5dr_devx_obj *
mlx5dr_cmd_stc_create(struct ibv_context *ctx,
		      struct mlx5dr_cmd_stc_create_attr *stc_attr)
{
	uint32_t out[MLX5_ST_SZ_DW(general_obj_out_cmd_hdr)] = {0};
	uint32_t in[MLX5_ST_SZ_DW(create_stc_in)] = {0};
	struct mlx5dr_devx_obj *devx_obj;
	void *attr;

	devx_obj = simple_malloc(sizeof(*devx_obj));
	if (!devx_obj) {
		DR_LOG(ERR, "Failed to allocate memory for STC object");
		rte_errno = ENOMEM;
		return NULL;
	}

	attr = MLX5_ADDR_OF(create_stc_in, in, hdr);
	MLX5_SET(general_obj_in_cmd_hdr,
		 attr, opcode, MLX5_CMD_OP_CREATE_GENERAL_OBJECT);
	MLX5_SET(general_obj_in_cmd_hdr,
		 attr, obj_type, MLX5_GENERAL_OBJ_TYPE_STC);
	MLX5_SET(general_obj_in_cmd_hdr,
		 attr, log_obj_range, stc_attr->log_obj_range);

	attr = MLX5_ADDR_OF(create_stc_in, in, stc);
	MLX5_SET(stc, attr, table_type, stc_attr->table_type);

	devx_obj->obj = mlx5_glue->devx_obj_create(ctx, in, sizeof(in), out, sizeof(out));
	if (!devx_obj->obj) {
		DR_LOG(ERR, "Failed to create STC (syndrome: %#x)",
		       mlx5dr_cmd_get_syndrome(out));
		simple_free(devx_obj);
		rte_errno = errno;
		return NULL;
	}

	devx_obj->id = MLX5_GET(general_obj_out_cmd_hdr, out, obj_id);

	return devx_obj;
}

static int
mlx5dr_cmd_stc_modify_set_stc_param(struct mlx5dr_cmd_stc_modify_attr *stc_attr,
				    void *stc_param)
{
	switch (stc_attr->action_type) {
	case MLX5_IFC_STC_ACTION_TYPE_COUNTER:
		MLX5_SET(stc_ste_param_flow_counter, stc_param, flow_counter_id, stc_attr->id);
		break;
	case MLX5_IFC_STC_ACTION_TYPE_JUMP_TO_TIR:
		MLX5_SET(stc_ste_param_tir, stc_param, tirn, stc_attr->dest_tir_num);
		break;
	case MLX5_IFC_STC_ACTION_TYPE_JUMP_TO_FT:
	case MLX5_IFC_STC_ACTION_TYPE_JUMP_TO_FLOW_TABLE_FDB_RX:
		MLX5_SET(stc_ste_param_table, stc_param, table_id, stc_attr->dest_table_id);
		break;
	case MLX5_IFC_STC_ACTION_TYPE_ACC_MODIFY_LIST:
		MLX5_SET(stc_ste_param_header_modify_list, stc_param,
			 header_modify_pattern_id, stc_attr->modify_header.pattern_id);
		MLX5_SET(stc_ste_param_header_modify_list, stc_param,
			 header_modify_argument_id, stc_attr->modify_header.arg_id);
		break;
	case MLX5_IFC_STC_ACTION_TYPE_HEADER_REMOVE:
		MLX5_SET(stc_ste_param_remove, stc_param, action_type,
			 MLX5_MODIFICATION_TYPE_REMOVE);
		MLX5_SET(stc_ste_param_remove, stc_param, decap,
			 stc_attr->remove_header.decap);
		MLX5_SET(stc_ste_param_remove, stc_param, remove_start_anchor,
			 stc_attr->remove_header.start_anchor);
		MLX5_SET(stc_ste_param_remove, stc_param, remove_end_anchor,
			 stc_attr->remove_header.end_anchor);
		break;
	case MLX5_IFC_STC_ACTION_TYPE_HEADER_INSERT:
		MLX5_SET(stc_ste_param_insert, stc_param, action_type,
			 MLX5_MODIFICATION_TYPE_INSERT);
		MLX5_SET(stc_ste_param_insert, stc_param, encap,
			 stc_attr->insert_header.encap);
		MLX5_SET(stc_ste_param_insert, stc_param, push_esp,
			 stc_attr->insert_header.push_esp);
		MLX5_SET(stc_ste_param_insert, stc_param, inline_data,
			 stc_attr->insert_header.is_inline);
		MLX5_SET(stc_ste_param_insert, stc_param, insert_anchor,
			 stc_attr->insert_header.insert_anchor);
		/* HW gets the next 2 sizes in words */
		MLX5_SET(stc_ste_param_insert, stc_param, insert_size,
			 stc_attr->insert_header.header_size / W_SIZE);
		MLX5_SET(stc_ste_param_insert, stc_param, insert_offset,
			 stc_attr->insert_header.insert_offset / W_SIZE);
		MLX5_SET(stc_ste_param_insert, stc_param, insert_argument,
			 stc_attr->insert_header.arg_id);
		break;
	case MLX5_IFC_STC_ACTION_TYPE_COPY:
	case MLX5_IFC_STC_ACTION_TYPE_SET:
	case MLX5_IFC_STC_ACTION_TYPE_ADD:
	case MLX5_IFC_STC_ACTION_TYPE_ADD_FIELD:
		*(__be64 *)stc_param = stc_attr->modify_action.data;
		break;
	case MLX5_IFC_STC_ACTION_TYPE_JUMP_TO_VPORT:
	case MLX5_IFC_STC_ACTION_TYPE_JUMP_TO_UPLINK:
		MLX5_SET(stc_ste_param_vport, stc_param, vport_number,
			 stc_attr->vport.vport_num);
		MLX5_SET(stc_ste_param_vport, stc_param, eswitch_owner_vhca_id,
			 stc_attr->vport.esw_owner_vhca_id);
		MLX5_SET(stc_ste_param_vport, stc_param, eswitch_owner_vhca_id_valid,
			 stc_attr->vport.eswitch_owner_vhca_id_valid);
		break;
	case MLX5_IFC_STC_ACTION_TYPE_DROP:
	case MLX5_IFC_STC_ACTION_TYPE_NOP:
	case MLX5_IFC_STC_ACTION_TYPE_TAG:
	case MLX5_IFC_STC_ACTION_TYPE_ALLOW:
		break;
	case MLX5_IFC_STC_ACTION_TYPE_ASO:
		MLX5_SET(stc_ste_param_execute_aso, stc_param, aso_object_id,
			 stc_attr->aso.devx_obj_id);
		MLX5_SET(stc_ste_param_execute_aso, stc_param, return_reg_id,
			 stc_attr->aso.return_reg_id);
		MLX5_SET(stc_ste_param_execute_aso, stc_param, aso_type,
			 stc_attr->aso.aso_type);
		break;
	case MLX5_IFC_STC_ACTION_TYPE_JUMP_TO_STE_TABLE:
		MLX5_SET(stc_ste_param_ste_table, stc_param, ste_obj_id,
			 stc_attr->ste_table.ste_obj_id);
		MLX5_SET(stc_ste_param_ste_table, stc_param, match_definer_id,
			 stc_attr->ste_table.match_definer_id);
		MLX5_SET(stc_ste_param_ste_table, stc_param, log_hash_size,
			 stc_attr->ste_table.log_hash_size);
		break;
	case MLX5_IFC_STC_ACTION_TYPE_REMOVE_WORDS:
		MLX5_SET(stc_ste_param_remove_words, stc_param, action_type,
			 MLX5_MODIFICATION_TYPE_REMOVE_WORDS);
		MLX5_SET(stc_ste_param_remove_words, stc_param, remove_start_anchor,
			 stc_attr->remove_words.start_anchor);
		MLX5_SET(stc_ste_param_remove_words, stc_param,
			 remove_size, stc_attr->remove_words.num_of_words);
		break;
	default:
		DR_LOG(ERR, "Not supported type %d", stc_attr->action_type);
		rte_errno = EINVAL;
		return rte_errno;
	}
	return 0;
}

int
mlx5dr_cmd_stc_modify(struct mlx5dr_devx_obj *devx_obj,
		      struct mlx5dr_cmd_stc_modify_attr *stc_attr)
{
	uint32_t out[MLX5_ST_SZ_DW(general_obj_out_cmd_hdr)] = {0};
	uint32_t in[MLX5_ST_SZ_DW(create_stc_in)] = {0};
	void *stc_param;
	void *attr;
	int ret;

	attr = MLX5_ADDR_OF(create_stc_in, in, hdr);
	MLX5_SET(general_obj_in_cmd_hdr,
		 attr, opcode, MLX5_CMD_OP_MODIFY_GENERAL_OBJECT);
	MLX5_SET(general_obj_in_cmd_hdr,
		 attr, obj_type, MLX5_GENERAL_OBJ_TYPE_STC);
	MLX5_SET(general_obj_in_cmd_hdr, in, obj_id, devx_obj->id);
	MLX5_SET(general_obj_in_cmd_hdr, in, obj_offset, stc_attr->stc_offset);

	attr = MLX5_ADDR_OF(create_stc_in, in, stc);
	MLX5_SET(stc, attr, ste_action_offset, stc_attr->action_offset);
	MLX5_SET(stc, attr, action_type, stc_attr->action_type);
	MLX5_SET(stc, attr, reparse_mode, stc_attr->reparse_mode);
	MLX5_SET64(stc, attr, modify_field_select,
		   MLX5_IFC_MODIFY_STC_FIELD_SELECT_NEW_STC);

	/* Set destination TIRN, TAG, FT ID, STE ID */
	stc_param = MLX5_ADDR_OF(stc, attr, stc_param);
	ret = mlx5dr_cmd_stc_modify_set_stc_param(stc_attr, stc_param);
	if (ret)
		return ret;

	ret = mlx5_glue->devx_obj_modify(devx_obj->obj, in, sizeof(in), out, sizeof(out));
	if (ret) {
		DR_LOG(ERR, "Failed to modify STC FW action_type %d (syndrome: %#x)",
		       stc_attr->action_type, mlx5dr_cmd_get_syndrome(out));
		rte_errno = errno;
	}

	return ret;
}

struct mlx5dr_devx_obj *
mlx5dr_cmd_arg_create(struct ibv_context *ctx,
		      uint16_t log_obj_range,
		      uint32_t pd)
{
	uint32_t out[MLX5_ST_SZ_DW(general_obj_out_cmd_hdr)] = {0};
	uint32_t in[MLX5_ST_SZ_DW(create_arg_in)] = {0};
	struct mlx5dr_devx_obj *devx_obj;
	void *attr;

	devx_obj = simple_malloc(sizeof(*devx_obj));
	if (!devx_obj) {
		DR_LOG(ERR, "Failed to allocate memory for ARG object");
		rte_errno = ENOMEM;
		return NULL;
	}

	attr = MLX5_ADDR_OF(create_arg_in, in, hdr);
	MLX5_SET(general_obj_in_cmd_hdr,
		 attr, opcode, MLX5_CMD_OP_CREATE_GENERAL_OBJECT);
	MLX5_SET(general_obj_in_cmd_hdr,
		 attr, obj_type, MLX5_GENERAL_OBJ_TYPE_ARG);
	MLX5_SET(general_obj_in_cmd_hdr,
		 attr, log_obj_range, log_obj_range);

	attr = MLX5_ADDR_OF(create_arg_in, in, arg);
	MLX5_SET(arg, attr, access_pd, pd);

	devx_obj->obj = mlx5_glue->devx_obj_create(ctx, in, sizeof(in), out, sizeof(out));
	if (!devx_obj->obj) {
		DR_LOG(ERR, "Failed to create ARG (syndrome: %#x)",
		       mlx5dr_cmd_get_syndrome(out));
		simple_free(devx_obj);
		rte_errno = errno;
		return NULL;
	}

	devx_obj->id = MLX5_GET(general_obj_out_cmd_hdr, out, obj_id);

	return devx_obj;
}

struct mlx5dr_devx_obj *
mlx5dr_cmd_header_modify_pattern_create(struct ibv_context *ctx,
					uint32_t pattern_length,
					uint8_t *actions)
{
	uint32_t in[MLX5_ST_SZ_DW(create_header_modify_pattern_in)] = {0};
	uint32_t out[MLX5_ST_SZ_DW(general_obj_out_cmd_hdr)] = {0};
	struct mlx5dr_devx_obj *devx_obj;
	uint64_t *pattern_data;
	int num_of_actions;
	void *pattern;
	void *attr;
	int i;

	if (pattern_length > MAX_ACTIONS_DATA_IN_HEADER_MODIFY) {
		DR_LOG(ERR, "Pattern length %d exceeds limit %d",
			pattern_length, MAX_ACTIONS_DATA_IN_HEADER_MODIFY);
		rte_errno = EINVAL;
		return NULL;
	}

	devx_obj = simple_malloc(sizeof(*devx_obj));
	if (!devx_obj) {
		DR_LOG(ERR, "Failed to allocate memory for header_modify_pattern object");
		rte_errno = ENOMEM;
		return NULL;
	}
	attr = MLX5_ADDR_OF(create_header_modify_pattern_in, in, hdr);
	MLX5_SET(general_obj_in_cmd_hdr,
		 attr, opcode, MLX5_CMD_OP_CREATE_GENERAL_OBJECT);
	MLX5_SET(general_obj_in_cmd_hdr,
		 attr, obj_type, MLX5_GENERAL_OBJ_TYPE_MODIFY_HEADER_PATTERN);

	pattern = MLX5_ADDR_OF(create_header_modify_pattern_in, in, pattern);
	/* Pattern_length is in ddwords */
	MLX5_SET(header_modify_pattern_in, pattern, pattern_length, pattern_length / (2 * DW_SIZE));

	pattern_data = (uint64_t *)MLX5_ADDR_OF(header_modify_pattern_in, pattern, pattern_data);
	memcpy(pattern_data, actions, pattern_length);

	num_of_actions = pattern_length / MLX5DR_MODIFY_ACTION_SIZE;
	for (i = 0; i < num_of_actions; i++) {
		int type;

		type = MLX5_GET(set_action_in, &pattern_data[i], action_type);
		if (type != MLX5_MODIFICATION_TYPE_COPY &&
		    type != MLX5_MODIFICATION_TYPE_ADD_FIELD)
			/* Action typ-copy use all bytes for control */
			MLX5_SET(set_action_in, &pattern_data[i], data, 0);
	}

	devx_obj->obj = mlx5_glue->devx_obj_create(ctx, in, sizeof(in), out, sizeof(out));
	if (!devx_obj->obj) {
		DR_LOG(ERR, "Failed to create header_modify_pattern (syndrome: %#x)",
		       mlx5dr_cmd_get_syndrome(out));
		rte_errno = errno;
		goto free_obj;
	}

	devx_obj->id = MLX5_GET(general_obj_out_cmd_hdr, out, obj_id);

	return devx_obj;

free_obj:
	simple_free(devx_obj);
	return NULL;
}

struct mlx5dr_devx_obj *
mlx5dr_cmd_ste_create(struct ibv_context *ctx,
		      struct mlx5dr_cmd_ste_create_attr *ste_attr)
{
	uint32_t out[MLX5_ST_SZ_DW(general_obj_out_cmd_hdr)] = {0};
	uint32_t in[MLX5_ST_SZ_DW(create_ste_in)] = {0};
	struct mlx5dr_devx_obj *devx_obj;
	void *attr;

	devx_obj = simple_malloc(sizeof(*devx_obj));
	if (!devx_obj) {
		DR_LOG(ERR, "Failed to allocate memory for STE object");
		rte_errno = ENOMEM;
		return NULL;
	}

	attr = MLX5_ADDR_OF(create_ste_in, in, hdr);
	MLX5_SET(general_obj_in_cmd_hdr,
		 attr, opcode, MLX5_CMD_OP_CREATE_GENERAL_OBJECT);
	MLX5_SET(general_obj_in_cmd_hdr,
		 attr, obj_type, MLX5_GENERAL_OBJ_TYPE_STE);
	MLX5_SET(general_obj_in_cmd_hdr,
		 attr, log_obj_range, ste_attr->log_obj_range);

	attr = MLX5_ADDR_OF(create_ste_in, in, ste);
	MLX5_SET(ste, attr, table_type, ste_attr->table_type);

	devx_obj->obj = mlx5_glue->devx_obj_create(ctx, in, sizeof(in), out, sizeof(out));
	if (!devx_obj->obj) {
		DR_LOG(ERR, "Failed to create STE (syndrome: %#x)",
		       mlx5dr_cmd_get_syndrome(out));
		simple_free(devx_obj);
		rte_errno = errno;
		return NULL;
	}

	devx_obj->id = MLX5_GET(general_obj_out_cmd_hdr, out, obj_id);

	return devx_obj;
}

struct mlx5dr_devx_obj *
mlx5dr_cmd_definer_create(struct ibv_context *ctx,
			  struct mlx5dr_cmd_definer_create_attr *def_attr)
{
	uint32_t out[MLX5_ST_SZ_DW(general_obj_out_cmd_hdr)] = {0};
	uint32_t in[MLX5_ST_SZ_DW(create_definer_in)] = {0};
	struct mlx5dr_devx_obj *devx_obj;
	void *ptr;

	devx_obj = simple_malloc(sizeof(*devx_obj));
	if (!devx_obj) {
		DR_LOG(ERR, "Failed to allocate memory for definer object");
		rte_errno = ENOMEM;
		return NULL;
	}

	MLX5_SET(general_obj_in_cmd_hdr,
		 in, opcode, MLX5_CMD_OP_CREATE_GENERAL_OBJECT);
	MLX5_SET(general_obj_in_cmd_hdr,
		 in, obj_type, MLX5_GENERAL_OBJ_TYPE_DEFINER);

	ptr = MLX5_ADDR_OF(create_definer_in, in, definer);
	MLX5_SET(definer, ptr, format_id, MLX5_IFC_DEFINER_FORMAT_ID_SELECT);

	MLX5_SET(definer, ptr, format_select_dw0, def_attr->dw_selector[0]);
	MLX5_SET(definer, ptr, format_select_dw1, def_attr->dw_selector[1]);
	MLX5_SET(definer, ptr, format_select_dw2, def_attr->dw_selector[2]);
	MLX5_SET(definer, ptr, format_select_dw3, def_attr->dw_selector[3]);
	MLX5_SET(definer, ptr, format_select_dw4, def_attr->dw_selector[4]);
	MLX5_SET(definer, ptr, format_select_dw5, def_attr->dw_selector[5]);
	MLX5_SET(definer, ptr, format_select_dw6, def_attr->dw_selector[6]);
	MLX5_SET(definer, ptr, format_select_dw7, def_attr->dw_selector[7]);
	MLX5_SET(definer, ptr, format_select_dw8, def_attr->dw_selector[8]);

	MLX5_SET(definer, ptr, format_select_byte0, def_attr->byte_selector[0]);
	MLX5_SET(definer, ptr, format_select_byte1, def_attr->byte_selector[1]);
	MLX5_SET(definer, ptr, format_select_byte2, def_attr->byte_selector[2]);
	MLX5_SET(definer, ptr, format_select_byte3, def_attr->byte_selector[3]);
	MLX5_SET(definer, ptr, format_select_byte4, def_attr->byte_selector[4]);
	MLX5_SET(definer, ptr, format_select_byte5, def_attr->byte_selector[5]);
	MLX5_SET(definer, ptr, format_select_byte6, def_attr->byte_selector[6]);
	MLX5_SET(definer, ptr, format_select_byte7, def_attr->byte_selector[7]);

	ptr = MLX5_ADDR_OF(definer, ptr, match_mask);
	memcpy(ptr, def_attr->match_mask, MLX5_FLD_SZ_BYTES(definer, match_mask));

	devx_obj->obj = mlx5_glue->devx_obj_create(ctx, in, sizeof(in), out, sizeof(out));
	if (!devx_obj->obj) {
		DR_LOG(ERR, "Failed to create Definer (syndrome: %#x)",
		       mlx5dr_cmd_get_syndrome(out));
		simple_free(devx_obj);
		rte_errno = errno;
		return NULL;
	}

	devx_obj->id = MLX5_GET(general_obj_out_cmd_hdr, out, obj_id);

	return devx_obj;
}

struct mlx5dr_devx_obj *
mlx5dr_cmd_sq_create(struct ibv_context *ctx,
		     struct mlx5dr_cmd_sq_create_attr *attr)
{
	uint32_t out[MLX5_ST_SZ_DW(create_sq_out)] = {0};
	uint32_t in[MLX5_ST_SZ_DW(create_sq_in)] = {0};
	void *sqc = MLX5_ADDR_OF(create_sq_in, in, ctx);
	void *wqc = MLX5_ADDR_OF(sqc, sqc, wq);
	struct mlx5dr_devx_obj *devx_obj;

	devx_obj = simple_malloc(sizeof(*devx_obj));
	if (!devx_obj) {
		DR_LOG(ERR, "Failed to create SQ");
		rte_errno = ENOMEM;
		return NULL;
	}

	MLX5_SET(create_sq_in, in, opcode, MLX5_CMD_OP_CREATE_SQ);
	MLX5_SET(sqc, sqc, cqn, attr->cqn);
	MLX5_SET(sqc, sqc, flush_in_error_en, 1);
	MLX5_SET(sqc, sqc, non_wire, 1);
	MLX5_SET(sqc, sqc, ts_format, attr->ts_format);
	MLX5_SET(wq, wqc, wq_type, MLX5_WQ_TYPE_CYCLIC);
	MLX5_SET(wq, wqc, pd, attr->pdn);
	MLX5_SET(wq, wqc, uar_page, attr->page_id);
	MLX5_SET(wq, wqc, log_wq_stride, log2above(MLX5_SEND_WQE_BB));
	MLX5_SET(wq, wqc, log_wq_sz, attr->log_wq_sz);
	MLX5_SET(wq, wqc, dbr_umem_id, attr->dbr_id);
	MLX5_SET(wq, wqc, wq_umem_id, attr->wq_id);

	devx_obj->obj = mlx5_glue->devx_obj_create(ctx, in, sizeof(in), out, sizeof(out));
	if (!devx_obj->obj) {
		simple_free(devx_obj);
		rte_errno = errno;
		return NULL;
	}

	devx_obj->id = MLX5_GET(create_sq_out, out, sqn);

	return devx_obj;
}

struct mlx5dr_devx_obj *
mlx5dr_cmd_packet_reformat_create(struct ibv_context *ctx,
				  struct mlx5dr_cmd_packet_reformat_create_attr *attr)
{
	uint32_t out[MLX5_ST_SZ_DW(alloc_packet_reformat_out)] = {0};
	size_t insz, cmd_data_sz, cmd_total_sz;
	struct mlx5dr_devx_obj *devx_obj;
	void *prctx;
	void *pdata;
	void *in;

	cmd_total_sz = MLX5_ST_SZ_BYTES(alloc_packet_reformat_context_in);
	cmd_total_sz += MLX5_ST_SZ_BYTES(packet_reformat_context_in);
	cmd_data_sz = MLX5_FLD_SZ_BYTES(packet_reformat_context_in, reformat_data);
	insz = align(cmd_total_sz + attr->data_sz - cmd_data_sz, DW_SIZE);
	in = simple_calloc(1, insz);
	if (!in) {
		rte_errno = ENOMEM;
		return NULL;
	}

	MLX5_SET(alloc_packet_reformat_context_in, in, opcode,
		 MLX5_CMD_OP_ALLOC_PACKET_REFORMAT_CONTEXT);

	prctx = MLX5_ADDR_OF(alloc_packet_reformat_context_in, in,
			     packet_reformat_context);
	pdata = MLX5_ADDR_OF(packet_reformat_context_in, prctx, reformat_data);

	MLX5_SET(packet_reformat_context_in, prctx, reformat_type, attr->type);
	MLX5_SET(packet_reformat_context_in, prctx, reformat_param_0, attr->reformat_param_0);
	MLX5_SET(packet_reformat_context_in, prctx, reformat_data_size, attr->data_sz);
	memcpy(pdata, attr->data, attr->data_sz);

	devx_obj = simple_malloc(sizeof(*devx_obj));
	if (!devx_obj) {
		DR_LOG(ERR, "Failed to allocate memory for packet reformat object");
		rte_errno = ENOMEM;
		goto out_free_in;
	}

	devx_obj->obj = mlx5_glue->devx_obj_create(ctx, in, insz, out, sizeof(out));
	if (!devx_obj->obj) {
		DR_LOG(ERR, "Failed to create packet reformat");
		rte_errno = errno;
		goto out_free_devx;
	}

	devx_obj->id = MLX5_GET(alloc_packet_reformat_out, out, packet_reformat_id);

	simple_free(in);

	return devx_obj;

out_free_devx:
	simple_free(devx_obj);
out_free_in:
	simple_free(in);
	return NULL;
}

int mlx5dr_cmd_sq_modify_rdy(struct mlx5dr_devx_obj *devx_obj)
{
	uint32_t out[MLX5_ST_SZ_DW(modify_sq_out)] = {0};
	uint32_t in[MLX5_ST_SZ_DW(modify_sq_in)] = {0};
	void *sqc = MLX5_ADDR_OF(modify_sq_in, in, ctx);
	int ret;

	MLX5_SET(modify_sq_in, in, opcode, MLX5_CMD_OP_MODIFY_SQ);
	MLX5_SET(modify_sq_in, in, sqn, devx_obj->id);
	MLX5_SET(modify_sq_in, in, sq_state, MLX5_SQC_STATE_RST);
	MLX5_SET(sqc, sqc, state, MLX5_SQC_STATE_RDY);

	ret = mlx5_glue->devx_obj_modify(devx_obj->obj, in, sizeof(in), out, sizeof(out));
	if (ret) {
		DR_LOG(ERR, "Failed to modify SQ (syndrome: %#x)",
		       mlx5dr_cmd_get_syndrome(out));
		rte_errno = errno;
	}

	return ret;
}

int mlx5dr_cmd_allow_other_vhca_access(struct ibv_context *ctx,
				       struct mlx5dr_cmd_allow_other_vhca_access_attr *attr)
{
	uint32_t out[MLX5_ST_SZ_DW(allow_other_vhca_access_out)] = {0};
	uint32_t in[MLX5_ST_SZ_DW(allow_other_vhca_access_in)] = {0};
	void *key;
	int ret;

	MLX5_SET(allow_other_vhca_access_in,
		 in, opcode, MLX5_CMD_OP_ALLOW_OTHER_VHCA_ACCESS);
	MLX5_SET(allow_other_vhca_access_in,
		 in, object_type_to_be_accessed, attr->obj_type);
	MLX5_SET(allow_other_vhca_access_in,
		 in, object_id_to_be_accessed, attr->obj_id);

	key = MLX5_ADDR_OF(allow_other_vhca_access_in, in, access_key);
	memcpy(key, attr->access_key, sizeof(attr->access_key));

	ret = mlx5_glue->devx_general_cmd(ctx, in, sizeof(in), out, sizeof(out));
	if (ret) {
		DR_LOG(ERR, "Failed to execute ALLOW_OTHER_VHCA_ACCESS command");
		rte_errno = errno;
		return rte_errno;
	}

	return 0;
}

struct mlx5dr_devx_obj *
mlx5dr_cmd_alias_obj_create(struct ibv_context *ctx,
			    struct mlx5dr_cmd_alias_obj_create_attr *alias_attr)
{
	uint32_t out[MLX5_ST_SZ_DW(general_obj_out_cmd_hdr)] = {0};
	uint32_t in[MLX5_ST_SZ_DW(create_alias_obj_in)] = {0};
	struct mlx5dr_devx_obj *devx_obj;
	void *attr;
	void *key;

	devx_obj = simple_malloc(sizeof(*devx_obj));
	if (!devx_obj) {
		DR_LOG(ERR, "Failed to allocate memory for ALIAS general object");
		rte_errno = ENOMEM;
		return NULL;
	}

	attr = MLX5_ADDR_OF(create_alias_obj_in, in, hdr);
	MLX5_SET(general_obj_in_cmd_hdr,
		 attr, opcode, MLX5_CMD_OP_CREATE_GENERAL_OBJECT);
	MLX5_SET(general_obj_in_cmd_hdr,
		 attr, obj_type, alias_attr->obj_type);
	MLX5_SET(general_obj_in_cmd_hdr, attr, alias_object, 1);

	attr = MLX5_ADDR_OF(create_alias_obj_in, in, alias_ctx);
	MLX5_SET(alias_context, attr, vhca_id_to_be_accessed, alias_attr->vhca_id);
	MLX5_SET(alias_context, attr, object_id_to_be_accessed, alias_attr->obj_id);

	key = MLX5_ADDR_OF(alias_context, attr, access_key);
	memcpy(key, alias_attr->access_key, sizeof(alias_attr->access_key));

	devx_obj->obj = mlx5_glue->devx_obj_create(ctx, in, sizeof(in), out, sizeof(out));
	if (!devx_obj->obj) {
		DR_LOG(ERR, "Failed to create ALIAS OBJ (syndrome: %#x)",
		       mlx5dr_cmd_get_syndrome(out));
		simple_free(devx_obj);
		rte_errno = errno;
		return NULL;
	}

	devx_obj->id = MLX5_GET(general_obj_out_cmd_hdr, out, obj_id);

	return devx_obj;
}

int mlx5dr_cmd_generate_wqe(struct ibv_context *ctx,
			    struct mlx5dr_cmd_generate_wqe_attr *attr,
			    struct mlx5_cqe64 *ret_cqe)
{
	uint32_t out[MLX5_ST_SZ_DW(generate_wqe_out)] = {0};
	uint32_t in[MLX5_ST_SZ_DW(generate_wqe_in)] = {0};
	uint8_t status;
	void *ptr;
	int ret;

	MLX5_SET(generate_wqe_in, in, opcode, MLX5_CMD_OP_GENERATE_WQE);
	MLX5_SET(generate_wqe_in, in, pdn, attr->pdn);

	ptr = MLX5_ADDR_OF(generate_wqe_in, in, wqe_ctrl);
	memcpy(ptr, attr->wqe_ctrl, MLX5_FLD_SZ_BYTES(generate_wqe_in, wqe_ctrl));

	ptr = MLX5_ADDR_OF(generate_wqe_in, in, wqe_gta_ctrl);
	memcpy(ptr, attr->gta_ctrl, MLX5_FLD_SZ_BYTES(generate_wqe_in, wqe_gta_ctrl));

	ptr = MLX5_ADDR_OF(generate_wqe_in, in, wqe_gta_data_0);
	memcpy(ptr, attr->gta_data_0, MLX5_FLD_SZ_BYTES(generate_wqe_in, wqe_gta_data_0));

	if (attr->gta_data_1) {
		ptr = MLX5_ADDR_OF(generate_wqe_in, in, wqe_gta_data_1);
		memcpy(ptr, attr->gta_data_1, MLX5_FLD_SZ_BYTES(generate_wqe_in, wqe_gta_data_1));
	}

	ret = mlx5_glue->devx_general_cmd(ctx, in, sizeof(in), out, sizeof(out));
	if (ret) {
		DR_LOG(ERR, "Failed to write GTA WQE using FW (syndrome: %#x)",
		       mlx5dr_cmd_get_syndrome(out));
		rte_errno = errno;
		return rte_errno;
	}

	status = MLX5_GET(generate_wqe_out, out, status);
	if (status) {
		DR_LOG(ERR, "Invalid FW CQE status %d", status);
		rte_errno = EINVAL;
		return rte_errno;
	}

	ptr = MLX5_ADDR_OF(generate_wqe_out, out, cqe_data);
	memcpy(ret_cqe, ptr, sizeof(*ret_cqe));

	return 0;
}

int mlx5dr_cmd_query_caps(struct ibv_context *ctx,
			  struct mlx5dr_cmd_query_caps *caps)
{
	uint32_t out[MLX5_ST_SZ_DW(query_hca_cap_out)] = {0};
	uint32_t in[MLX5_ST_SZ_DW(query_hca_cap_in)] = {0};
	const struct flow_hw_port_info *port_info;
	struct ibv_device_attr_ex attr_ex;
	u32 res;
	int ret;

	MLX5_SET(query_hca_cap_in, in, opcode, MLX5_CMD_OP_QUERY_HCA_CAP);
	MLX5_SET(query_hca_cap_in, in, op_mod,
		 MLX5_GET_HCA_CAP_OP_MOD_GENERAL_DEVICE |
		 MLX5_HCA_CAP_OPMOD_GET_CUR);

	ret = mlx5_glue->devx_general_cmd(ctx, in, sizeof(in), out, sizeof(out));
	if (ret) {
		DR_LOG(ERR, "Failed to query device caps");
		rte_errno = errno;
		return rte_errno;
	}

	caps->wqe_based_update =
		MLX5_GET(query_hca_cap_out, out,
			 capability.cmd_hca_cap.wqe_based_flow_table_update_cap);

	caps->eswitch_manager = MLX5_GET(query_hca_cap_out, out,
					 capability.cmd_hca_cap.eswitch_manager);

	caps->flex_protocols = MLX5_GET(query_hca_cap_out, out,
					capability.cmd_hca_cap.flex_parser_protocols);

	caps->log_header_modify_argument_granularity =
		MLX5_GET(query_hca_cap_out, out,
			 capability.cmd_hca_cap.log_header_modify_argument_granularity);

	caps->log_header_modify_argument_granularity -=
			MLX5_GET(query_hca_cap_out, out,
				 capability.cmd_hca_cap.
				 log_header_modify_argument_granularity_offset);

	caps->log_header_modify_argument_max_alloc =
		MLX5_GET(query_hca_cap_out, out,
			 capability.cmd_hca_cap.log_header_modify_argument_max_alloc);

	caps->definer_format_sup =
		MLX5_GET64(query_hca_cap_out, out,
			   capability.cmd_hca_cap.match_definer_format_supported);

	caps->vhca_id = MLX5_GET(query_hca_cap_out, out,
				 capability.cmd_hca_cap.vhca_id);

	caps->sq_ts_format = MLX5_GET(query_hca_cap_out, out,
				      capability.cmd_hca_cap.sq_ts_format);

	caps->ipsec_offload = MLX5_GET(query_hca_cap_out, out,
				      capability.cmd_hca_cap.ipsec_offload);

	caps->roce = MLX5_GET(query_hca_cap_out, out, capability.cmd_hca_cap.roce);

	MLX5_SET(query_hca_cap_in, in, op_mod,
		 MLX5_GET_HCA_CAP_OP_MOD_GENERAL_DEVICE_2 |
		 MLX5_HCA_CAP_OPMOD_GET_CUR);

	ret = mlx5_glue->devx_general_cmd(ctx, in, sizeof(in), out, sizeof(out));
	if (ret) {
		DR_LOG(ERR, "Failed to query device caps");
		rte_errno = errno;
		return rte_errno;
	}

	caps->full_dw_jumbo_support = MLX5_GET(query_hca_cap_out, out,
					       capability.cmd_hca_cap_2.
					       format_select_dw_8_6_ext);

	caps->format_select_gtpu_dw_0 = MLX5_GET(query_hca_cap_out, out,
						 capability.cmd_hca_cap_2.
						 format_select_dw_gtpu_dw_0);

	caps->format_select_gtpu_dw_1 = MLX5_GET(query_hca_cap_out, out,
						 capability.cmd_hca_cap_2.
						 format_select_dw_gtpu_dw_1);

	caps->format_select_gtpu_dw_2 = MLX5_GET(query_hca_cap_out, out,
						 capability.cmd_hca_cap_2.
						 format_select_dw_gtpu_dw_2);

	caps->format_select_gtpu_ext_dw_0 = MLX5_GET(query_hca_cap_out, out,
						     capability.cmd_hca_cap_2.
						     format_select_dw_gtpu_first_ext_dw_0);

	caps->supp_type_gen_wqe = MLX5_GET(query_hca_cap_out, out,
					   capability.cmd_hca_cap_2.
					   generate_wqe_type);

	/* check cross-VHCA support in cap2 */
	res =
	MLX5_GET(query_hca_cap_out, out,
		capability.cmd_hca_cap_2.cross_vhca_object_to_object_supported);

	caps->cross_vhca_resources = (res & MLX5_CROSS_VHCA_OBJ_TO_OBJ_TYPE_STC_TO_TIR) &&
				     (res & MLX5_CROSS_VHCA_OBJ_TO_OBJ_TYPE_STC_TO_FT) &&
				     (res & MLX5_CROSS_VHCA_OBJ_TO_OBJ_TYPE_FT_TO_RTC);

	res =
	MLX5_GET(query_hca_cap_out, out,
		capability.cmd_hca_cap_2.allowed_object_for_other_vhca_access);

	caps->cross_vhca_resources &= (res & MLX5_CROSS_VHCA_ALLOWED_OBJS_TIR) &&
				      (res & MLX5_CROSS_VHCA_ALLOWED_OBJS_FT) &&
				      (res & MLX5_CROSS_VHCA_ALLOWED_OBJS_RTC);

	caps->flow_table_hash_type = MLX5_GET(query_hca_cap_out, out,
					      capability.cmd_hca_cap_2.flow_table_hash_type);

	caps->encap_entropy_hash_type = MLX5_GET(query_hca_cap_out, out,
						 capability.cmd_hca_cap_2.encap_entropy_hash_type);

	MLX5_SET(query_hca_cap_in, in, op_mod,
		 MLX5_GET_HCA_CAP_OP_MOD_NIC_FLOW_TABLE |
		 MLX5_HCA_CAP_OPMOD_GET_CUR);

	ret = mlx5_glue->devx_general_cmd(ctx, in, sizeof(in), out, sizeof(out));
	if (ret) {
		DR_LOG(ERR, "Failed to query flow table caps");
		rte_errno = errno;
		return rte_errno;
	}

	caps->nic_ft.max_level = MLX5_GET(query_hca_cap_out, out,
					  capability.flow_table_nic_cap.
					  flow_table_properties_nic_receive.max_ft_level);

	caps->nic_ft.reparse = MLX5_GET(query_hca_cap_out, out,
					capability.flow_table_nic_cap.
					flow_table_properties_nic_receive.reparse);

	caps->nic_ft.ignore_flow_level_rtc_valid =
		MLX5_GET(query_hca_cap_out,
			 out,
			 capability.flow_table_nic_cap.
			 flow_table_properties_nic_receive.ignore_flow_level_rtc_valid);

	/* check cross-VHCA support in flow table properties */
	res =
	MLX5_GET(query_hca_cap_out, out,
		capability.flow_table_nic_cap.flow_table_properties_nic_receive.cross_vhca_object);
	caps->cross_vhca_resources &= res;

	if (caps->wqe_based_update) {
		MLX5_SET(query_hca_cap_in, in, op_mod,
			 MLX5_GET_HCA_CAP_OP_MOD_WQE_BASED_FLOW_TABLE |
			 MLX5_HCA_CAP_OPMOD_GET_CUR);

		ret = mlx5_glue->devx_general_cmd(ctx, in, sizeof(in), out, sizeof(out));
		if (ret) {
			DR_LOG(ERR, "Failed to query WQE based FT caps");
			rte_errno = errno;
			return rte_errno;
		}

		caps->rtc_reparse_mode = MLX5_GET(query_hca_cap_out, out,
						  capability.wqe_based_flow_table_cap.
						  rtc_reparse_mode);

		caps->ste_format = MLX5_GET(query_hca_cap_out, out,
					    capability.wqe_based_flow_table_cap.
					    ste_format);

		caps->rtc_index_mode = MLX5_GET(query_hca_cap_out, out,
						capability.wqe_based_flow_table_cap.
						rtc_index_mode);

		caps->rtc_log_depth_max = MLX5_GET(query_hca_cap_out, out,
						   capability.wqe_based_flow_table_cap.
						   rtc_log_depth_max);

		caps->ste_alloc_log_max = MLX5_GET(query_hca_cap_out, out,
						   capability.wqe_based_flow_table_cap.
						   ste_alloc_log_max);

		caps->ste_alloc_log_gran = MLX5_GET(query_hca_cap_out, out,
						    capability.wqe_based_flow_table_cap.
						    ste_alloc_log_granularity);

		caps->trivial_match_definer = MLX5_GET(query_hca_cap_out, out,
						       capability.wqe_based_flow_table_cap.
						       trivial_match_definer);

		caps->stc_alloc_log_max = MLX5_GET(query_hca_cap_out, out,
						   capability.wqe_based_flow_table_cap.
						   stc_alloc_log_max);

		caps->stc_alloc_log_gran = MLX5_GET(query_hca_cap_out, out,
						    capability.wqe_based_flow_table_cap.
						    stc_alloc_log_granularity);

		caps->rtc_hash_split_table = MLX5_GET(query_hca_cap_out, out,
						      capability.wqe_based_flow_table_cap.
						      rtc_hash_split_table);

		caps->rtc_linear_lookup_table = MLX5_GET(query_hca_cap_out, out,
							 capability.wqe_based_flow_table_cap.
							 rtc_linear_lookup_table);

		caps->access_index_mode = MLX5_GET(query_hca_cap_out, out,
						   capability.wqe_based_flow_table_cap.
						   access_index_mode);

		caps->linear_match_definer = MLX5_GET(query_hca_cap_out, out,
						      capability.wqe_based_flow_table_cap.
						      linear_match_definer_reg_c3);

		caps->rtc_max_hash_def_gen_wqe = MLX5_GET(query_hca_cap_out, out,
							  capability.wqe_based_flow_table_cap.
							  rtc_max_num_hash_definer_gen_wqe);

		caps->supp_ste_format_gen_wqe = MLX5_GET(query_hca_cap_out, out,
							 capability.wqe_based_flow_table_cap.
							 ste_format_gen_wqe);

		caps->fdb_tir_stc = MLX5_GET(query_hca_cap_out, out,
					     capability.wqe_based_flow_table_cap.
					     fdb_jump_to_tir_stc);

		caps->fdb_unified_en = MLX5_GET(query_hca_cap_out, out,
						capability.wqe_based_flow_table_cap.
						fdb_unified_en);

		caps->stc_action_type_63_0 = MLX5_GET64(query_hca_cap_out,
							out,
					capability.wqe_based_flow_table_cap.stc_action_type_63_0);

		caps->stc_action_type_127_64 = MLX5_GET64(query_hca_cap_out,
							  out,
					capability.wqe_based_flow_table_cap.stc_action_type_127_64);
	}

	if (caps->eswitch_manager) {
		MLX5_SET(query_hca_cap_in, in, op_mod,
			 MLX5_GET_HCA_CAP_OP_MOD_ESW_FLOW_TABLE |
			 MLX5_HCA_CAP_OPMOD_GET_CUR);

		ret = mlx5_glue->devx_general_cmd(ctx, in, sizeof(in), out, sizeof(out));
		if (ret) {
			DR_LOG(ERR, "Failed to query flow table esw caps");
			rte_errno = errno;
			return rte_errno;
		}

		caps->fdb_ft.max_level = MLX5_GET(query_hca_cap_out, out,
						  capability.flow_table_nic_cap.
						  flow_table_properties_nic_receive.max_ft_level);

		caps->fdb_ft.reparse = MLX5_GET(query_hca_cap_out, out,
						capability.flow_table_nic_cap.
						flow_table_properties_nic_receive.reparse);

		MLX5_SET(query_hca_cap_in, in, op_mod,
			 MLX5_SET_HCA_CAP_OP_MOD_ESW | MLX5_HCA_CAP_OPMOD_GET_CUR);

		ret = mlx5_glue->devx_general_cmd(ctx, in, sizeof(in), out, sizeof(out));
		if (ret) {
			DR_LOG(ERR, "Query eswitch capabilities failed %d", ret);
			rte_errno = errno;
			return rte_errno;
		}

		if (MLX5_GET(query_hca_cap_out, out,
			     capability.esw_cap.esw_manager_vport_number_valid))
			caps->eswitch_manager_vport_number =
			MLX5_GET(query_hca_cap_out, out,
				 capability.esw_cap.esw_manager_vport_number);

		caps->merged_eswitch = MLX5_GET(query_hca_cap_out, out,
						capability.esw_cap.merged_eswitch);
	}

	if (caps->roce) {
		MLX5_SET(query_hca_cap_in, in, op_mod,
			 MLX5_GET_HCA_CAP_OP_MOD_ROCE |
			 MLX5_HCA_CAP_OPMOD_GET_CUR);

		ret = mlx5_glue->devx_general_cmd(ctx, in, sizeof(in), out, sizeof(out));
		if (ret) {
			DR_LOG(ERR, "Failed to query roce caps");
			rte_errno = errno;
			return rte_errno;
		}

		caps->roce_max_src_udp_port = MLX5_GET(query_hca_cap_out, out,
						capability.roce_caps.r_roce_max_src_udp_port);
		caps->roce_min_src_udp_port = MLX5_GET(query_hca_cap_out, out,
						capability.roce_caps.r_roce_min_src_udp_port);
	}

	ret = mlx5_glue->query_device_ex(ctx, NULL, &attr_ex);
	if (ret) {
		DR_LOG(ERR, "Failed to query device attributes");
		rte_errno = ret;
		return rte_errno;
	}

	strlcpy(caps->fw_ver, attr_ex.orig_attr.fw_ver, sizeof(caps->fw_ver));

	port_info = flow_hw_get_wire_port(ctx);
	if (port_info)
		caps->wire_regc_mask = port_info->regc_mask;
	else
		DR_LOG(INFO, "Failed to query wire port regc value");

	return ret;
}

int mlx5dr_cmd_query_ib_port(struct ibv_context *ctx,
			     struct mlx5dr_cmd_query_vport_caps *vport_caps,
			     uint32_t port_num)
{
	struct mlx5_port_info port_info = {0};
	uint32_t flags;
	int ret;

	flags = MLX5_PORT_QUERY_VPORT | MLX5_PORT_QUERY_ESW_OWNER_VHCA_ID;

	ret = mlx5_glue->devx_port_query(ctx, port_num, &port_info);
	/* Check if query succeed and vport is enabled */
	if (ret || (port_info.query_flags & flags) != flags) {
		rte_errno = ENOTSUP;
		return rte_errno;
	}

	vport_caps->vport_num = port_info.vport_id;
	vport_caps->esw_owner_vhca_id = port_info.esw_owner_vhca_id;

	return 0;
}
