/* radare - LGPL - Copyright 2011-2018 - pancake, h4ng3r */

#include <r_cons.h>
#include <r_types.h>
#include <r_util.h>
#include <r_lib.h>
#include <r_bin.h>
#include "dex/dex.h"
#define r_hash_adler32 __adler32
#include "../../hash/adler32.c"

/* method flags */
#define R_DEX_METH_PUBLIC 0x0001
#define R_DEX_METH_PRIVATE 0x0002
#define R_DEX_METH_PROTECTED 0x0004
#define R_DEX_METH_STATIC 0x0008
#define R_DEX_METH_FINAL 0x0010
#define R_DEX_METH_SYNCHRONIZED 0x0020
#define R_DEX_METH_BRIDGE 0x0040
#define R_DEX_METH_VARARGS 0x0080
#define R_DEX_METH_NATIVE 0x0100
#define R_DEX_METH_ABSTRACT 0x0400
#define R_DEX_METH_STRICT 0x0800
#define R_DEX_METH_SYNTHETIC 0x1000
#define R_DEX_METH_MIRANDA 0x8000
#define R_DEX_METH_CONSTRUCTOR 0x10000
#define R_DEX_METH_DECLARED_SYNCHRONIZED 0x20000
#define r_str_const(x) x

extern struct r_bin_dbginfo_t r_bin_dbginfo_dex;

static bool dexdump = false;
static Sdb *mdb = NULL;

static ut64 get_method_flags(ut64 MA);

static char *getstr(RBinDexObj *bin, int idx) {
	ut8 buf[6];
	ut64 len;
	int uleblen;
	// null terminate the buf wtf
	if (!bin || idx < 0 || idx >= bin->header.strings_size || !bin->strings) {
		return NULL;
	}
	if (bin->strings[idx] >= bin->size) {
		return NULL;
	}
	if (r_buf_read_at (bin->b, bin->strings[idx], buf, sizeof (buf)) < 1) {
		return NULL;
	}
	bin->b->buf[bin->b->length - 1] = 0;
	uleblen = r_uleb128 (buf, sizeof (buf), &len) - buf;
	if (!uleblen || uleblen >= bin->size) {
		return NULL;
	}
	if (!len || len >= bin->size) {
		return NULL;
	}
	if (bin->strings[idx] + uleblen >= bin->strings[idx] + bin->header.strings_size) {
		return NULL;
	}
	char* ptr = (char*) r_buf_get_at (bin->b, bin->strings[idx] + uleblen, NULL);
	if (!ptr) {
		return NULL;
	}

	if (len != r_utf8_strlen ((const ut8*)ptr)) {
		// eprintf ("WARNING: Invalid string for index %d\n", idx);
		return NULL;
	}
	return ptr;
}

static int countOnes(ut32 val) {
	/* visual studio doesnt supports __buitin_clz */
#ifdef _MSC_VER
	int count = 0;
	val = val - ((val >> 1) & 0x55555555);
	val = (val & 0x33333333) + ((val >> 2) & 0x33333333);
	count = (((val + (val >> 4)) & 0x0F0F0F0F) * 0x01010101) >> 24;
	return count;
#else
	return __builtin_clz (val);
#endif
}

typedef enum {
	kAccessForClass  = 0,
	kAccessForMethod = 1,
	kAccessForField  = 2,
	kAccessForMAX
} AccessFor;

static char *createAccessFlagStr(ut32 flags, AccessFor forWhat) {
	#define NUM_FLAGS 18
	static const char* kAccessStrings[kAccessForMAX][NUM_FLAGS] = {
		{
			/* class, inner class */
			"PUBLIC",           /* 0x0001 */
			"PRIVATE",          /* 0x0002 */
			"PROTECTED",        /* 0x0004 */
			"STATIC",           /* 0x0008 */
			"FINAL",            /* 0x0010 */
			"?",                /* 0x0020 */
			"?",                /* 0x0040 */
			"?",                /* 0x0080 */
			"?",                /* 0x0100 */
			"INTERFACE",        /* 0x0200 */
			"ABSTRACT",         /* 0x0400 */
			"?",                /* 0x0800 */
			"SYNTHETIC",        /* 0x1000 */
			"ANNOTATION",       /* 0x2000 */
			"ENUM",             /* 0x4000 */
			"?",                /* 0x8000 */
			"VERIFIED",         /* 0x10000 */
			"OPTIMIZED",        /* 0x20000 */
		},
		{
			/* method */
			"PUBLIC",           /* 0x0001 */
			"PRIVATE",          /* 0x0002 */
			"PROTECTED",        /* 0x0004 */
			"STATIC",           /* 0x0008 */
			"FINAL",            /* 0x0010 */
			"SYNCHRONIZED",     /* 0x0020 */
			"BRIDGE",           /* 0x0040 */
			"VARARGS",          /* 0x0080 */
			"NATIVE",           /* 0x0100 */
			"?",                /* 0x0200 */
			"ABSTRACT",         /* 0x0400 */
			"STRICT",           /* 0x0800 */
			"SYNTHETIC",        /* 0x1000 */
			"?",                /* 0x2000 */
			"?",                /* 0x4000 */
			"MIRANDA",          /* 0x8000 */
			"CONSTRUCTOR",      /* 0x10000 */
			"DECLARED_SYNCHRONIZED", /* 0x20000 */
		},
		{
			/* field */
			"PUBLIC",           /* 0x0001 */
			"PRIVATE",          /* 0x0002 */
			"PROTECTED",        /* 0x0004 */
			"STATIC",           /* 0x0008 */
			"FINAL",            /* 0x0010 */
			"?",                /* 0x0020 */
			"VOLATILE",         /* 0x0040 */
			"TRANSIENT",        /* 0x0080 */
			"?",                /* 0x0100 */
			"?",                /* 0x0200 */
			"?",                /* 0x0400 */
			"?",                /* 0x0800 */
			"SYNTHETIC",        /* 0x1000 */
			"?",                /* 0x2000 */
			"ENUM",             /* 0x4000 */
			"?",                /* 0x8000 */
			"?",                /* 0x10000 */
			"?",                /* 0x20000 */
		},
	};
	int i, count = countOnes (flags);
	const int kLongest = 21;
	const int maxSize = (count + 1) * (kLongest + 1);
	char* str, *cp;
	// produces a huge number????
	cp = str = (char*) calloc (count + 1, (kLongest + 1));
	if (!str) {
		return NULL;
	}
	if (count == 0) {
		*cp = '\0';
		return cp;
	}
	for (i = 0; i < NUM_FLAGS; i++) {
		if (flags & 0x01) {
			const char *accessStr = kAccessStrings[forWhat][i];
			int len = strlen (accessStr);
			if (cp != str) {
				*cp++ = ' ';
			}
			if (((cp - str) + len) >= maxSize) {
				return NULL;
			}
			memcpy (cp, accessStr, len);
			cp += len;
		}
		flags >>= 1;
	}
	*cp = '\0';
	return str;
}

static char *dex_type_descriptor(RBinDexObj *bin, int type_idx) {
	if (type_idx < 0 || type_idx >= bin->header.types_size) {
		return NULL;
	}
	return getstr (bin, bin->types[type_idx].descriptor_id);
}

static char *dex_get_proto(RBinDexObj *bin, int proto_id) {
	ut32 params_off, type_id, list_size;
	char *r = NULL, *return_type = NULL, *signature = NULL, *buff = NULL;
	ut8 *bufptr;
	ut16 type_idx;
	int pos = 0, i, size = 1;

	if (proto_id >= bin->header.prototypes_size) {
		return NULL;
	}
	params_off = bin->protos[proto_id].parameters_off;
	if (params_off >= bin->size) {
		return NULL;
	}
	type_id = bin->protos[proto_id].return_type_id;
	if (type_id >= bin->header.types_size ) {
		return NULL;
	}
	return_type = getstr (bin, bin->types[type_id].descriptor_id);
	if (!return_type) {
		return NULL;
	}
	if (!params_off) {
		return r_str_newf ("()%s", return_type);;
	}
	ut8 params_buf[sizeof (ut32)];
	if (!r_buf_read_at (bin->b, params_off, params_buf, sizeof (params_buf))) {
		return NULL;
	}
	// size of the list, in entries
	list_size = r_read_le32 (params_buf);
	if (list_size * sizeof (ut16) >= bin->size) {
		return NULL;
	}

	for (i = 0; i < list_size; i++) {
		int buff_len = 0;
		int off = params_off + 4 + (i * 2);
		if (off >= bin->size) {
			break;
		}
		ut8 typeidx_buf[sizeof (ut16)];
		if (!r_buf_read_at (bin->b, off, typeidx_buf, sizeof (typeidx_buf))) {
			break;
		}
		type_idx = r_read_le16 (typeidx_buf);
		if (type_idx >= bin->header.types_size || type_idx >= bin->size) {
			break;
		}
		buff = getstr (bin, bin->types[type_idx].descriptor_id);
		if (!buff) {
			break;
		}
		buff_len = strlen (buff);
		size += buff_len + 1;
		char *newsig = realloc (signature, size);
		if (!newsig) {
			eprintf ("Cannot realloc to %d\n", size);
			break;
		}
		signature = newsig;
		strcpy (signature + pos, buff);
		pos += buff_len;
		signature[pos] = '\0';
	}
	if (signature) {
		r = r_str_newf ("(%s)%s", signature, return_type);
		free (signature);
	}
	return r;
}

static char *dex_method_signature(RBinDexObj *bin, int method_idx) {
	if (method_idx < 0 || method_idx >= bin->header.method_size) {
		return NULL;
	}
	return dex_get_proto (bin, bin->methods[method_idx].proto_id);
}

static RList *dex_method_signature2(RBinDexObj *bin, int method_idx) {
	ut32 proto_id, params_off, list_size;
	char *buff = NULL;
	ut8 *bufptr;
	ut16 type_idx;
	int i;

	RList *params = r_list_newf (free);
	if (!params) {
		return NULL;
	}
	if (method_idx < 0 || method_idx >= bin->header.method_size) {
		goto out_error;
	}
	proto_id = bin->methods[method_idx].proto_id;
	if (proto_id >= bin->header.prototypes_size) {
		goto out_error;
	}
	params_off = bin->protos[proto_id].parameters_off;
	if (params_off  >= bin->size) {
		goto out_error;
	}
	if (!params_off) {
		return params;
	}
	bufptr = bin->b->buf;
	// size of the list, in entries
	list_size = r_read_le32 (bufptr + params_off);
	//XXX list_size tainted it may produce huge loop
	for (i = 0; i < list_size; i++) {
		ut64 of = params_off + 4 + (i * 2);
		if (of >= bin->size || of < params_off) {
			break;
		}
		type_idx = r_read_le16 (bufptr + of);
		if (type_idx >= bin->header.types_size ||
		    type_idx > bin->size) {
			break;
		}
		buff = getstr (bin, bin->types[type_idx].descriptor_id);
		if (!buff) {
			break;
		}
		r_list_append (params, buff);
	}
	return params;
out_error:
	r_list_free (params);
	return NULL;
}

// TODO: fix this, now has more registers that it should
// https://github.com/android/platform_dalvik/blob/0641c2b4836fae3ee8daf6c0af45c316c84d5aeb/libdex/DexDebugInfo.cpp#L312
// https://github.com/android/platform_dalvik/blob/0641c2b4836fae3ee8daf6c0af45c316c84d5aeb/libdex/DexDebugInfo.cpp#L141
static void dex_parse_debug_item(RBinFile *binfile, RBinDexObj *bin,
				  RBinDexClass *c, int MI, int MA, int paddr, int ins_size,
				  int insns_size, char *class_name, int regsz,
				  int debug_info_off) {
	struct r_bin_t *rbin = binfile->rbin;
	struct r_bin_dex_obj_t *dex = binfile->o->bin_obj;
	const ut8 *p4 = r_buf_get_at (binfile->buf, debug_info_off, NULL);
	const ut8 *p4_end = p4 + binfile->buf->length - debug_info_off;
	ut64 line_start;
	ut64 parameters_size;
	ut64 param_type_idx;
	ut16 argReg = regsz - ins_size;
	ut64 source_file_idx = c->source_file;
	RList *params, *debug_positions, *emitted_debug_locals = NULL;
	bool keep = true;
	if (argReg > regsz) {
		return; // this return breaks tests
	}
	p4 = r_uleb128 (p4, p4_end - p4, &line_start);
	p4 = r_uleb128 (p4, p4_end - p4, &parameters_size);
	// TODO: check when we should use source_file
	// The state machine consists of five registers
	ut32 address = 0;
	ut32 line = line_start;
	if (!(debug_positions = r_list_newf ((RListFree)free))) {
		return;
	}
	if (!(emitted_debug_locals = r_list_newf ((RListFree)free))) {
		free (debug_positions);
		return;
	}

	struct dex_debug_local_t *debug_locals = calloc (sizeof (struct dex_debug_local_t), regsz + 1);
	if (!(MA & 0x0008)) {
		debug_locals[argReg].name = "this";
		debug_locals[argReg].descriptor = r_str_newf ("%s;", class_name);
		debug_locals[argReg].startAddress = 0;
		debug_locals[argReg].signature = NULL;
		debug_locals[argReg].live = true;
		argReg++;
	}
	if (!(params = dex_method_signature2 (bin, MI))) {
		free (debug_positions);
		free (emitted_debug_locals);
		free (debug_locals);
		return;
	}

	RListIter *iter;
	char *name;
	char *type;
	int reg;

	r_list_foreach (params, iter, type) {
		if ((argReg >= regsz) || !type || parameters_size <= 0) {
			free (debug_positions);
			free (params);
			free (debug_locals);
			free (emitted_debug_locals);
			return;
		}
		p4 = r_uleb128 (p4, p4_end - p4, &param_type_idx); // read uleb128p1
		param_type_idx -= 1;
		name = getstr (bin, param_type_idx);
		reg = argReg;
		switch (type[0]) {
		case 'D':
		case 'J':
			argReg += 2;
			break;
		default:
			argReg += 1;
			break;
		}
		if (!name || !*name) {
			debug_locals[reg].name = name;
			debug_locals[reg].descriptor = type;
			debug_locals[reg].signature = NULL;
			debug_locals[reg].startAddress = address;
			debug_locals[reg].live = true;
		}
		parameters_size--;
	}

	if (!p4 || p4 >= p4_end) {
		free (debug_positions);
		free (params);
		free (debug_locals);
		free (emitted_debug_locals);
		return;
	}
	ut8 opcode = *(p4++) & 0xff;
	while (keep) {
		switch (opcode) {
		case 0x0: // DBG_END_SEQUENCE
			keep = false;
			break;
		case 0x1: // DBG_ADVANCE_PC
			{
			ut64 addr_diff;
			p4 = r_uleb128 (p4, p4_end - p4, &addr_diff);
			address += addr_diff;
			}
			break;
		case 0x2: // DBG_ADVANCE_LINE
			{
			st64 line_diff = r_sleb128 (&p4, p4_end);
			line += line_diff;
			}
			break;
		case 0x3: // DBG_START_LOCAL
			{
			ut64 register_num;
			ut64 name_idx;
			ut64 type_idx;
			p4 = r_uleb128 (p4, p4_end - p4, &register_num);
			p4 = r_uleb128 (p4, p4_end - p4, &name_idx);
			name_idx -= 1;
			p4 = r_uleb128 (p4, p4_end - p4, &type_idx);
			type_idx -= 1;
			if (register_num >= regsz) {
				r_list_free (debug_positions);
				free (params);
				free (debug_locals);
				free (emitted_debug_locals);
				return;
			}
			// Emit what was previously there, if anything
			// emitLocalCbIfLive
			if (debug_locals[register_num].live) {
				struct dex_debug_local_t *local = malloc (
					sizeof (struct dex_debug_local_t));
				if (!local) {
					keep = false;
					break;
				}
				local->name = debug_locals[register_num].name;
				local->descriptor = debug_locals[register_num].descriptor;
				local->startAddress = debug_locals[register_num].startAddress;
				local->signature = debug_locals[register_num].signature;
				local->live = true;
				local->reg = register_num;
				local->endAddress = address;
				r_list_append (emitted_debug_locals, local);
			}
			debug_locals[register_num].name = getstr (bin, name_idx);
			debug_locals[register_num].descriptor = dex_type_descriptor (bin, type_idx);
			debug_locals[register_num].startAddress = address;
			debug_locals[register_num].signature = NULL;
			debug_locals[register_num].live = true;
			//eprintf("DBG_START_LOCAL %x %x %x\n", register_num, name_idx, type_idx);
			}
			break;
		case 0x4: //DBG_START_LOCAL_EXTENDED
			{
			ut64 register_num, name_idx, type_idx, sig_idx;
			p4 = r_uleb128 (p4, p4_end - p4, &register_num);
			p4 = r_uleb128 (p4, p4_end - p4, &name_idx);
			name_idx -= 1;
			p4 = r_uleb128 (p4, p4_end - p4, &type_idx);
			type_idx -= 1;
			p4 = r_uleb128 (p4, p4_end - p4, &sig_idx);
			sig_idx -= 1;
			if (register_num >= regsz) {
				r_list_free (debug_positions);
				free (params);
				free (debug_locals);
				return;
			}

			// Emit what was previously there, if anything
			// emitLocalCbIfLive
			if (debug_locals[register_num].live) {
				struct dex_debug_local_t *local = malloc (
					sizeof (struct dex_debug_local_t));
				if (!local) {
					keep = false;
					break;
				}
				local->name = debug_locals[register_num].name;
				local->descriptor = debug_locals[register_num].descriptor;
				local->startAddress = debug_locals[register_num].startAddress;
				local->signature = debug_locals[register_num].signature;
				local->live = true;
				local->reg = register_num;
				local->endAddress = address;
				r_list_append (emitted_debug_locals, local);
			}

			debug_locals[register_num].name = getstr (bin, name_idx);
			debug_locals[register_num].descriptor = dex_type_descriptor (bin, type_idx);
			debug_locals[register_num].startAddress = address;
			debug_locals[register_num].signature = getstr (bin, sig_idx);
			debug_locals[register_num].live = true;
			}
			break;
		case 0x5: // DBG_END_LOCAL
			{
			ut64 register_num;
			p4 = r_uleb128 (p4, p4_end - p4, &register_num);
			// emitLocalCbIfLive
			if (register_num >= regsz) {
				r_list_free (debug_positions);
				free (params);
				free (debug_locals);
				return;
			}
			if (debug_locals[register_num].live) {
				struct dex_debug_local_t *local = malloc (
					sizeof (struct dex_debug_local_t));
				if (!local) {
					keep = false;
					break;
				}
				local->name = debug_locals[register_num].name;
				local->descriptor = debug_locals[register_num].descriptor;
				local->startAddress = debug_locals[register_num].startAddress;
				local->signature = debug_locals[register_num].signature;
				local->live = true;
				local->reg = register_num;
				local->endAddress = address;
				r_list_append (emitted_debug_locals, local);
			}
			debug_locals[register_num].live = false;
			}
			break;
		case 0x6: // DBG_RESTART_LOCAL
			{
			ut64 register_num;
			p4 = r_uleb128 (p4, p4_end - p4, &register_num);
			if (register_num >= regsz) {
				r_list_free (debug_positions);
				free (params);
				free (debug_locals);
				return;
			}
			if (!debug_locals[register_num].live) {
				debug_locals[register_num].startAddress = address;
				debug_locals[register_num].live = true;
			}
			}
			break;
		case 0x7: //DBG_SET_PROLOGUE_END
			break;
		case 0x8: //DBG_SET_PROLOGUE_BEGIN
			break;
		case 0x9:
			{
			p4 = r_uleb128 (p4, p4_end - p4, &source_file_idx);
			source_file_idx--;
			}
			break;
		default:
			{
			int adjusted_opcode = opcode - 10;
			address += (adjusted_opcode / 15);
			line += -4 + (adjusted_opcode % 15);
			struct dex_debug_position_t *position =
				malloc (sizeof (struct dex_debug_position_t));
			if (!position) {
				keep = false;
				break;
			}
			position->source_file_idx = source_file_idx;
			position->address = address;
			position->line = line;
			r_list_append (debug_positions, position);
			}
			break;
		}
		opcode = *(p4++) & 0xff;
	}

	if (!binfile->sdb_addrinfo) {
		binfile->sdb_addrinfo = sdb_new0 ();
	}


	RListIter *iter1;
	struct dex_debug_position_t *pos;
// Loading the debug info takes too much time and nobody uses this afaik
#if 1
	r_list_foreach (debug_positions, iter1, pos) {
		const char *line = getstr (bin, pos->source_file_idx);
#if 1
		char offset[64] = {0};
		if (!line || !*line) {
			continue;
		}
		char *fileline = r_str_newf ("%s|%"PFMT64d, line, pos->line);
		char *offset_ptr = sdb_itoa (pos->address + paddr, offset, 16);
		sdb_set (binfile->sdb_addrinfo, offset_ptr, fileline, 0);
		sdb_set (binfile->sdb_addrinfo, fileline, offset_ptr, 0);
		free (fileline);
#endif
		RBinDwarfRow *rbindwardrow = R_NEW0 (RBinDwarfRow);
		if (!rbindwardrow) {
			dexdump = false;
			break;
		}
		if (line) {
			rbindwardrow->file = strdup (line);
			rbindwardrow->address = pos->address;
			rbindwardrow->line = pos->line;
			r_list_append (dex->lines_list, rbindwardrow);
		}
	}
#endif

	if (!dexdump) {
		free (debug_positions);
		free (emitted_debug_locals);
		free (debug_locals);
		free (params);
		return;
	}

	RListIter *iter2;
	struct dex_debug_position_t *position;

	rbin->cb_printf ("      positions     :\n");
	r_list_foreach (debug_positions, iter2, position) {
		rbin->cb_printf ("        0x%04"PFMT64x" line=%llu\n",
				 position->address, position->line);
	}

	rbin->cb_printf ("      locals        :\n");

	RListIter *iter3;
	struct dex_debug_local_t *local;
	r_list_foreach (emitted_debug_locals, iter3, local) {
		if (local->signature) {
			rbin->cb_printf (
				"        0x%04x - 0x%04x reg=%d %s %s %s\n",
				local->startAddress, local->endAddress,
				local->reg, local->name, local->descriptor,
				local->signature);
		} else {
			rbin->cb_printf (
				"        0x%04x - 0x%04x reg=%d %s %s\n",
				local->startAddress, local->endAddress,
				local->reg, local->name, local->descriptor);
		}
	}

	for (reg = 0; reg < regsz; reg++) {
		if (!debug_locals[reg].name) {
			continue;
		}
		if (debug_locals[reg].live) {
			if (debug_locals[reg].signature) {
				rbin->cb_printf (
					"        0x%04x - 0x%04x reg=%d %s %s "
					"%s\n",
					debug_locals[reg].startAddress,
					insns_size, reg, debug_locals[reg].name,
					debug_locals[reg].descriptor,
					debug_locals[reg].signature);
			} else {
				rbin->cb_printf (
					"        0x%04x - 0x%04x reg=%d %s %s"
					"\n",
					debug_locals[reg].startAddress,
					insns_size, reg, debug_locals[reg].name,
					debug_locals[reg].descriptor);
			}
		}
	}
	free (debug_positions);
	free (debug_locals);
	free (emitted_debug_locals);
	free (params);
}

static Sdb *get_sdb (RBinFile *bf) {
	RBinObject *o = bf->o;
	if (!o || !o->bin_obj) {
		return NULL;
	}
	struct r_bin_dex_obj_t *bin = (struct r_bin_dex_obj_t *) o->bin_obj;
	return bin? bin->kv: NULL;
}

static void *load_bytes(RBinFile *bf, const ut8 *buf, ut64 sz, ut64 loadaddr, Sdb *sdb){
	void *res = NULL;
	RBuffer *tbuf = NULL;
	if (!buf || !sz || sz == UT64_MAX) {
		return NULL;
	}
	tbuf = r_buf_new ();
	if (!tbuf) {
		return NULL;
	}
	r_buf_set_bytes (tbuf, buf, sz);
	res = r_bin_dex_new_buf (tbuf);
	r_buf_free (tbuf);
	return res;
}

static void * load_buffer(RBinFile *bf, RBuffer *buf, ut64 loadaddr, Sdb *sdb) {
	return r_bin_dex_new_buf (buf);
}

static bool load(RBinFile *bf) {
	const ut8 *bytes = bf ? r_buf_buffer (bf->buf) : NULL;
	ut64 sz = bf ? r_buf_size (bf->buf): 0;

	if (!bf || !bf->o) {
		return false;
	}
	bf->o->bin_obj = load_bytes (bf, bytes, sz, bf->o->loadaddr, bf->sdb);
	return bf->o->bin_obj ? true: false;
}

static ut64 baddr(RBinFile *bf) {
	return 0;
}

static bool check_bytes(const ut8 *buf, ut64 length) {
	if (!buf || length < 8) {
		return false;
	}
	// Non-extended opcode dex file
	if (!memcmp (buf, "dex\n035\0", 8)) {
		return true;
	}
	// Extended (jumnbo) opcode dex file, ICS+ only (sdk level 14+)
	if (!memcmp (buf, "dex\n036\0", 8)) {
		return true;
	}
	// Two new opcodes: invoke-polymorphic and invoke-custom (sdk level 26+)
	if (!memcmp (buf, "dex\n038\0", 8)) {
		return true;
	}
	// M3 (Nov-Dec 07)
	if (!memcmp (buf, "dex\n009\0", 8)) {
		return true;
	}
	// M5 (Feb-Mar 08)
	if (!memcmp (buf, "dex\n009\0", 8)) {
		return true;
	}
	// Default fall through, should still be a dex file
	if (!memcmp (buf, "dex\n", 4)) {
		return true;
	}
	return false;
}

static RBinInfo *info(RBinFile *bf) {
	RBinHash *h;
	RBinInfo *ret = R_NEW0 (RBinInfo);
	if (!ret) {
		return NULL;
	}
	ret->file = bf->file? strdup (bf->file): NULL;
	ret->type = strdup ("DEX CLASS");
	ret->has_va = false;
	ret->has_lit = true;
	ret->bclass = r_bin_dex_get_version (bf->o->bin_obj);
	ret->rclass = strdup ("class");
	ret->os = strdup ("linux");
	const char *kw = "Landroid/support/wearable/view";
	if (r_mem_mem (bf->buf->buf, bf->buf->length, (const ut8*)kw, strlen (kw))) {
		ret->subsystem = strdup ("android-wear");
	} else {
		ret->subsystem = strdup ("android");
	}
	ret->machine = strdup ("Dalvik VM");
	h = &ret->sum[0];
	h->type = "sha1";
	h->len = 20;
	h->addr = 12;
	h->from = 12;
	h->to = bf->buf->length-32;
	memcpy (h->buf, bf->buf->buf + 12, 20);
	h = &ret->sum[1];
	h->type = "adler32";
	h->len = 4;
	h->addr = 0x8;
	h->from = 12;
	h->to = bf->buf->length-h->from;
	h = &ret->sum[2];
	h->type = 0;
	memcpy (h->buf, bf->buf->buf + 8, 4);
	{
		ut32 *fc = (ut32 *)(bf->buf->buf + 8);
		ut32  cc = __adler32 (bf->buf->buf + 12, bf->buf->length - 12);
		if (*fc != cc) {
			eprintf ("# adler32 checksum doesn't match. Type this to fix it:\n");
			eprintf ("wx `ph sha1 $s-32 @32` @12 ; wx `ph adler32 $s-12 @12` @8\n");
		}
	}
	ret->arch = strdup ("dalvik");
	ret->lang = "dalvik";
	ret->bits = 32;
	ret->big_endian = 0;
	ret->dbg_info = 0; //1 | 4 | 8; /* Stripped | LineNums | Syms */
	return ret;
}

static RList *strings(RBinFile *bf) {
	struct r_bin_dex_obj_t *bin = NULL;
	RBinString *ptr = NULL;
	RList *ret = NULL;
	int i, len;
	ut8 buf[6];
	ut64 off;
	if (!bf || !bf->o) {
		return NULL;
	}
	bin = (struct r_bin_dex_obj_t *) bf->o->bin_obj;
	if (!bin || !bin->strings) {
		return NULL;
	}
	if (bin->header.strings_size > bin->size) {
		bin->strings = NULL;
		return NULL;
	}
	if (!(ret = r_list_newf (free))) {
		return NULL;
	}
	for (i = 0; i < bin->header.strings_size; i++) {
		if (!(ptr = R_NEW0 (RBinString))) {
			break;
		}
		if (bin->strings[i] > bin->size || bin->strings[i] + 6 > bin->size) {
			goto out_error;
		}
		r_buf_read_at (bin->b, bin->strings[i], (ut8*)&buf, 6);
		len = dex_read_uleb128 (buf, sizeof (buf));

		if (len > 5 && len < R_BIN_SIZEOF_STRINGS) {
			ptr->string = malloc (len + 1);
			if (!ptr->string) {
				goto out_error;
			}
			off = bin->strings[i] + dex_uleb128_len (buf, sizeof (buf));
			if (off + len >= bin->size || off + len < len) {
				free (ptr->string);
				goto out_error;
			}
			r_buf_read_at (bin->b, off, (ut8*)ptr->string, len);
			ptr->string[len] = 0;
			if ((ptr->string[0] == 'L' && strchr (ptr->string, '/')) || !strncmp (ptr->string, "[L", 2)) {
				free (ptr->string);
				continue;
			}
			ptr->vaddr = ptr->paddr = bin->strings[i];
			ptr->size = len;
			ptr->length = len;
			ptr->ordinal = i+1;
			r_list_append (ret, ptr);
		} else {
			free (ptr);
		}
	}
	return ret;
out_error:
	r_list_free (ret);
	free (ptr);
	return NULL;
}

static char *dex_method_name(RBinDexObj *bin, int idx) {
	if (idx < 0 || idx >= bin->header.method_size) {
		return NULL;
	}
	int cid = bin->methods[idx].class_id;
	if (cid < 0 || cid >= bin->header.strings_size) {
		return NULL;
	}
	int tid = bin->methods[idx].name_id;
	if (tid < 0 || tid >= bin->header.strings_size) {
		return NULL;
	}
	return getstr (bin, tid);
}

static char *dex_class_name_byid(RBinDexObj *bin, int cid) {
	int tid;
	if (!bin || !bin->types) {
		return NULL;
	}
	if (cid < 0 || cid >= bin->header.types_size) {
		return NULL;
	}
	tid = bin->types[cid].descriptor_id;
	return getstr (bin, tid);
}

static char *dex_class_name(RBinDexObj *bin, RBinDexClass *c) {
	return dex_class_name_byid (bin, c->class_id);
}

static char *dex_field_name(RBinDexObj *bin, int fid) {
	int cid, tid, type_id;
	if (!bin || !bin->fields) {
		return NULL;
	}
	if (fid < 0 || fid >= bin->header.fields_size) {
		return NULL;
	}
	cid = bin->fields[fid].class_id;
	if (cid < 0 || cid >= bin->header.types_size) {
		return NULL;
	}
	type_id = bin->fields[fid].type_id;
	if (type_id < 0 || type_id >= bin->header.types_size) {
		return NULL;
	}
	tid = bin->fields[fid].name_id;
	return r_str_newf ("%s->%s %s", getstr (bin, bin->types[cid].descriptor_id),
		getstr (bin, tid), getstr (bin, bin->types[type_id].descriptor_id));
}

static char *dex_method_fullname(RBinDexObj *bin, int method_idx) {
	if (!bin || !bin->types) {
		return NULL;
	}
	if (method_idx < 0 || method_idx >= bin->header.method_size) {
		return NULL;
	}
	int cid = bin->methods[method_idx].class_id;
	if (cid < 0 || cid >= bin->header.types_size) {
		return NULL;
	}
	const char *name = dex_method_name (bin, method_idx);
	if (!name) {
		return NULL;
	}
	const char *className = dex_class_name_byid (bin, cid);
	char *flagname = NULL;
	if (className) {
		char *class_name = strdup (className);
		r_str_replace_char (class_name, ';', 0);
		char *signature = dex_method_signature (bin, method_idx);
		if (signature) {
			flagname = r_str_newf ("%s.%s%s", class_name, name, signature);
			free (signature);
		} else {
			flagname = r_str_newf ("%s.%s%s", class_name, name, "???");
		}
		free (class_name);
	} else {
		char *signature = dex_method_signature (bin, method_idx);
		if (signature) {
			flagname = r_str_newf ("%s.%s%s", "???", name, signature);
			free (signature);
		} else {
			flagname = r_str_newf ("%s.%s%s", "???", name, "???");
			free (signature);
		}
	}
	return flagname;
}

static ut64 dex_get_type_offset(RBinFile *bf, int type_idx) {
	RBinDexObj *bin = (RBinDexObj*) bf->o->bin_obj;
	if (!bin || !bin->types) {
		return 0;
	}
	if (type_idx < 0 || type_idx >= bin->header.types_size) {
		return 0;
	}
	return bin->header.types_offset + type_idx * 0x04; //&bin->types[type_idx];
}

static const char *dex_class_super_name(RBinDexObj *bin, RBinDexClass *c) {
	int cid, tid;
	if (!bin || !c || !bin->types) {
		return NULL;
	}
	cid = c->super_class;
	if (cid < 0 || cid >= bin->header.types_size) {
		return NULL;
	}
	tid = bin->types[cid].descriptor_id;
	return getstr (bin, tid);
}

static const ut8 *parse_dex_class_fields(RBinFile *binfile, RBinDexObj *bin,
					  RBinDexClass *c, RBinClass *cls,
					  const ut8 *p, const ut8 *p_end,
					  int *sym_count, ut64 fields_count,
					  bool is_sfield) {
	struct r_bin_t *rbin = binfile->rbin;
	ut64 lastIndex = 0;
	ut8 ff[sizeof (DexField)] = {0};
	int total, i, tid;
	DexField field;
	const char* type_str;
	for (i = 0; i < fields_count; i++) {
		ut64 fieldIndex, accessFlags;

		p = r_uleb128 (p, p_end - p, &fieldIndex); // fieldIndex
		p = r_uleb128 (p, p_end - p, &accessFlags); // accessFlags
		fieldIndex += lastIndex;
		total = bin->header.fields_offset + (sizeof (DexField) * fieldIndex);
		if (total >= bin->size || total < bin->header.fields_offset) {
			break;
		}
		if (r_buf_read_at (binfile->buf, total, ff,
				sizeof (DexField)) != sizeof (DexField)) {
			break;
		}
		field.class_id = r_read_le16 (ff);
		field.type_id = r_read_le16 (ff + 2);
		field.name_id = r_read_le32 (ff + 4);
		char *fieldName = getstr (bin, field.name_id);
		if (field.type_id >= bin->header.types_size) {
			break;
		}
		tid = bin->types[field.type_id].descriptor_id;
		type_str = getstr (bin, tid);
		RBinSymbol *sym = R_NEW0 (RBinSymbol);
		if (!sym) {
			return NULL;
		}
		if (is_sfield) {
			sym->name = r_str_newf ("%s.sfield_%s:%s", cls->name,
						fieldName, type_str);
			sym->type = r_str_const ("STATIC");
		} else {
			sym->name = r_str_newf ("%s.ifield_%s:%s", cls->name,
						fieldName, type_str);
			sym->type = r_str_const ("FIELD");
		}
		sym->name = r_str_replace (sym->name, "method.", "", 0);
		r_str_replace_char (sym->name, ';', 0);
		sym->paddr = sym->vaddr = total;
		sym->ordinal = (*sym_count)++;

		if (dexdump) {
			const char *accessStr = createAccessFlagStr (
				accessFlags, kAccessForField);
			rbin->cb_printf ("    #%d              : (in %s;)\n", i,
					 cls->name);
			rbin->cb_printf ("      name          : '%s'\n", fieldName);
			rbin->cb_printf ("      type          : '%s'\n", type_str);
			rbin->cb_printf ("      access        : 0x%04x (%s)\n",
					 (unsigned int)accessFlags, accessStr);
		}
		r_list_append (bin->methods_list, sym);

		RBinField *field = R_NEW0 (RBinField);
		if (!field) {
			return NULL;
		}
		field->vaddr = field->paddr = sym->paddr;
		field->name = strdup (sym->name);
		field->flags = get_method_flags (accessFlags);
		r_list_append (cls->fields, field);

		lastIndex = fieldIndex;
	}
	return p;
}

// TODO: refactor this method
// XXX it needs a lot of love!!!
static const ut8 *parse_dex_class_method(RBinFile *binfile, RBinDexObj *bin,
		RBinDexClass *c, RBinClass *cls,
		const ut8 *p, const ut8 *p_end,
		int *sym_count, ut64 DM, int *methods,
		bool is_direct) {
	struct r_bin_t *rbin = binfile->rbin;
	bool bin_dbginfo = rbin->want_dbginfo;
	int i, left;
	ut64 omi = 0;
	bool catchAll;
	ut16 regsz = 0, ins_size = 0, outs_size = 0, tries_size = 0;
	ut16 handler_off, start_addr, insn_count = 0;
	ut32 debug_info_off = 0, insns_size = 0;
	const ut8 *encoded_method_addr = NULL;

	if (DM > 4096) {
		eprintf ("This DEX is probably corrupted. Chopping DM from %d to 4KB\n", (int)DM);
		DM = 4096;
	}
	for (i = 0; i < DM; i++) {
		encoded_method_addr = p;
		char *method_name, *flag_name;
		ut64 MI, MA, MC;
		p = r_uleb128 (p, p_end - p, &MI);
		MI += omi;
		omi = MI;
		p = r_uleb128 (p, p_end - p, &MA);
		p = r_uleb128 (p, p_end - p, &MC);
		// TODO: MOVE CHECKS OUTSIDE!
		if (MI < bin->header.method_size) {
			if (methods) {
				methods[MI] = 1;
			}
		}
		method_name = dex_method_name (bin, MI);
		char *signature = dex_method_signature (bin, MI);
		if (!method_name) {
			method_name = "unknown";
		}
		flag_name = r_str_newf ("%s.method.%s%s", cls->name, method_name, signature);
		if (!flag_name || !*flag_name) {
			//R_FREE (method_name);
			R_FREE (flag_name);
			R_FREE (signature);
			continue;
		}
		// TODO: check size
		// ut64 prolog_size = 2 + 2 + 2 + 2 + 4 + 4;
		ut64 v2, handler_type, handler_addr;
		int t = 0;
		if (MC > 0) {
			// TODO: parse debug info
			// XXX why binfile->buf->base???
			if (MC + 16 >= bin->size || MC + 16 < MC) {
				//R_FREE (method_name);
				R_FREE (flag_name);
				R_FREE (signature);
				continue;
			}
			const ut8 *ff2 = r_buf_get_at (binfile->buf, binfile->buf->base + MC, &left);
			if (!ff2 || left < 16) {
				//R_FREE (method_name);
				R_FREE (flag_name);
				R_FREE (signature);
				continue;
			}
			regsz = r_read_le16 (ff2);
			ins_size = r_read_le16 (ff2 + 2);
			outs_size = r_read_le16 (ff2 + 4);
			tries_size = r_read_le16 (ff2 + 6);
			debug_info_off = r_read_le32 (ff2 + 8);
			insns_size = r_read_le32 (ff2 + 12);
			int padd = 0;
			if (tries_size > 0 && insns_size % 2) {
				padd = 2;
			}
			t = 16 + 2 * insns_size + padd;
		}
		if (dexdump) {
			const char* accessStr = createAccessFlagStr (MA, kAccessForMethod);
			rbin->cb_printf ("    #%d              : (in %s;)\n", i, cls->name);
			rbin->cb_printf ("      name          : '%s'\n", method_name);
			rbin->cb_printf ("      type          : '%s'\n", signature);
			rbin->cb_printf ("      access        : 0x%04x (%s)\n",
					 (unsigned int)MA, accessStr);
		}

		if (MC > 0) {
			if (dexdump) {
				rbin->cb_printf ("      code          -\n");
				rbin->cb_printf ("      registers     : %d\n", regsz);
				rbin->cb_printf ("      ins           : %d\n", ins_size);
				rbin->cb_printf ("      outs          : %d\n", outs_size);
				rbin->cb_printf (
					"      insns size    : %d 16-bit code "
					"units\n",
					insns_size);
			}
			if (tries_size > 0) {
				if (dexdump) {
					rbin->cb_printf ("      catches       : %d\n", tries_size);
				}
				int j, m = 0;
				//XXX bucle controlled by tainted variable it could produces huge loop
				for (j = 0; j < tries_size; ++j) {
					ut64 offset = MC + t + j * 8;
					if (offset >= bin->size || offset < MC) {
						R_FREE (signature);
						break;
					}
					const ut8 *ptr = r_buf_get_at (binfile->buf, binfile->buf->base + offset, &left);
					if (!ptr || left < 8) {
						R_FREE (signature);
						break;
					}
					start_addr = r_read_le32 (ptr);
					insn_count = r_read_le16 (ptr + 4);
					handler_off = r_read_le16 (ptr + 6);

					char* s = NULL;
					if (dexdump) {
						rbin->cb_printf ("        0x%04x - 0x%04x\n",
							start_addr, (start_addr + insn_count));
					}

					//XXX tries_size is tainted and oob here
					int off = MC + t + tries_size * 8 + handler_off;
					if (off >= bin->size || off < tries_size) {
						R_FREE (signature);
						break;
					}
					// TODO: catch left instead of null
					const ut8 *p3 = r_buf_get_at (binfile->buf, off, NULL);
					const ut8 *p3_end = p3 + binfile->buf->length - off;
					st64 size = r_sleb128 (&p3, p3_end);

					if (size <= 0) {
						catchAll = true;
						size = -size;
						// XXX this is probably wrong
					} else {
						catchAll = false;
					}

					for (m = 0; m < size; m++) {
						p3 = r_uleb128 (p3, p3_end - p3, &handler_type);
						p3 = r_uleb128 (p3, p3_end - p3, &handler_addr);

						if (handler_type > 0 && handler_type < bin->header.types_size) {
							s = getstr (bin, bin->types[handler_type].descriptor_id);
							if (dexdump) {
								rbin->cb_printf (
									"          %s "
									"-> 0x%04"PFMT64x"\n",
									s,
									handler_addr);
							}
						} else {
							if (dexdump) {
								rbin->cb_printf ("          (error) -> 0x%04"PFMT64x"\n", handler_addr);
							}
						}
					}
					if (catchAll) {
						p3 = r_uleb128 (p3, p3_end - p3, &v2);
						if (dexdump) {
							rbin->cb_printf ("          <any> -> 0x%04"PFMT64x"\n", v2);
						}
					}
				}
			} else {
				if (dexdump) {
					rbin->cb_printf (
						"      catches       : "
						"(none)\n");
				}
			}
		} else {
			if (dexdump) {
				rbin->cb_printf (
					"      code          : (none)\n");
			}
		}
		if (*flag_name) {
			RBinSymbol *sym = R_NEW0 (RBinSymbol);
			if (!sym) {
				R_FREE (flag_name);
				return NULL;
			}
			sym->name = flag_name;
			// is_direct is no longer used
			// if method has code *addr points to code
			// otherwise it points to the encoded method
			if (MC > 0) {
				sym->type = r_str_const (R_BIN_TYPE_FUNC_STR);
				sym->paddr = MC;// + 0x10;
				sym->vaddr = MC;// + 0x10;
			} else {
				sym->type = r_str_const ("METH");
				sym->paddr = encoded_method_addr - binfile->buf->buf;
				sym->vaddr = encoded_method_addr - binfile->buf->buf;
			}
			if ((MA & 0x1) == 0x1) {
				sym->bind = r_str_const (R_BIN_BIND_GLOBAL_STR);
			} else {
				sym->bind = r_str_const (R_BIN_BIND_LOCAL_STR);
			}

			sym->method_flags = get_method_flags (MA);

			sym->ordinal = (*sym_count)++;
			if (MC > 0) {
				const ut8 *ff2 = r_buf_get_at (binfile->buf, binfile->buf->base + MC, &left);
				if (!ff2 || left < 16) {
				// if (r_buf_read_at (binfile->buf, binfile->buf->base + MC, ff2, 16) < 1) {
					R_FREE (sym);
					R_FREE (signature);
					continue;
				}
				//ut16 regsz = r_read_le16 (ff2);
				//ut16 ins_size = r_read_le16 (ff2 + 2);
				//ut16 outs_size = r_read_le16 (ff2 + 4);
				ut16 tries_size = r_read_le16 (ff2 + 6);
				//ut32 debug_info_off = r_read_le32 (ff2 + 8);
				ut32 insns_size = r_read_le32 (ff2 + 12);
				ut64 prolog_size = 2 + 2 + 2 + 2 + 4 + 4;
				if (tries_size > 0) {
					//prolog_size += 2 + 8*tries_size; // we need to parse all so the catch info...
				}
				// TODO: prolog_size
				sym->paddr = MC + prolog_size;// + 0x10;
				sym->vaddr = MC + prolog_size;// + 0x10;
				//if (is_direct) {
				sym->size = insns_size * 2;
				//}
				//eprintf("%s (0x%x-0x%x) size=%d\nregsz=%d\ninsns_size=%d\nouts_size=%d\ntries_size=%d\ninsns_size=%d\n", flag_name, sym->vaddr, sym->vaddr+sym->size, prolog_size, regsz, ins_size, outs_size, tries_size, insns_size);
				r_list_append (bin->methods_list, sym);
				r_list_append (cls->methods, sym);

				if (bin->code_from > sym->paddr) {
					bin->code_from = sym->paddr;
				}
				if (bin->code_to < sym->paddr) {
					bin->code_to = sym->paddr;
				}

				if (!mdb) {
					mdb = sdb_new0 ();
				}
				sdb_num_set (mdb, sdb_fmt ("method.%d", MI), sym->paddr, 0);
				// -----------------
				// WORK IN PROGRESS
				// -----------------
#if 0
				if (0) {
					if (MA & 0x10000) { //ACC_CONSTRUCTOR
						if (!cdb) {
							cdb = sdb_new0 ();
						}
						sdb_num_set (cdb, sdb_fmt ("%d", c->class_id), sym->paddr, 0);
					}
				}
#endif
			} else {
				sym->size = 0;
				r_list_append (bin->methods_list, sym);
				r_list_append (cls->methods, sym);
			}
			if (MC > 0 && debug_info_off > 0 && bin->header.data_offset < debug_info_off &&
				debug_info_off < bin->header.data_offset + bin->header.data_size) {
				if (bin_dbginfo) {
					dex_parse_debug_item (binfile, bin, c, MI, MA, sym->paddr, ins_size,
							insns_size, cls->name, regsz, debug_info_off);
				}
			} else if (MC > 0) {
				if (dexdump) {
					rbin->cb_printf ("      positions     :\n");
					rbin->cb_printf ("      locals        :\n");
				}
			}
		} else {
			R_FREE (flag_name);
		}
		R_FREE (signature);
		//R_FREE (method_name);
	}
	return p;
}

static ut64 get_method_flags(ut64 MA) {
	ut64 flags = 0;
	if (MA & R_DEX_METH_PUBLIC) {
		flags |= R_BIN_METH_PUBLIC;
	}
	if (MA & R_DEX_METH_PRIVATE) {
		flags |= R_BIN_METH_PRIVATE;
	}
	if (MA & R_DEX_METH_PROTECTED) {
		flags |= R_BIN_METH_PROTECTED;
	}
	if (MA & R_DEX_METH_STATIC) {
		flags |= R_BIN_METH_STATIC;
	}
	if (MA & R_DEX_METH_FINAL) {
		flags |= R_BIN_METH_FINAL;
	}
	if (MA & R_DEX_METH_SYNCHRONIZED) {
		flags |= R_BIN_METH_SYNCHRONIZED;
	}
	if (MA & R_DEX_METH_BRIDGE) {
		flags |= R_BIN_METH_BRIDGE;
	}
	if (MA & R_DEX_METH_VARARGS) {
		flags |= R_BIN_METH_VARARGS;
	}
	if (MA & R_DEX_METH_NATIVE) {
		flags |= R_BIN_METH_NATIVE;
	}
	if (MA & R_DEX_METH_ABSTRACT) {
		flags |= R_BIN_METH_ABSTRACT;
	}
	if (MA & R_DEX_METH_STRICT) {
		flags |= R_BIN_METH_STRICT;
	}
	if (MA & R_DEX_METH_SYNTHETIC) {
		flags |= R_BIN_METH_SYNTHETIC;
	}
	if (MA & R_DEX_METH_MIRANDA) {
		flags |= R_BIN_METH_MIRANDA;
	}
	if (MA & R_DEX_METH_CONSTRUCTOR) {
		flags |= R_BIN_METH_CONSTRUCTOR;
	}
	if (MA & R_DEX_METH_DECLARED_SYNCHRONIZED) {
		flags |= R_BIN_METH_DECLARED_SYNCHRONIZED;
	}
	return flags;
}

static void parse_class(RBinFile *binfile, RBinDexObj *bin, RBinDexClass *c,
			 int class_index, int *methods, int *sym_count) {
	struct r_bin_t *rbin = binfile->rbin;

	char *class_name;
	int z;
	const ut8 *p, *p_end;

	if (!c) {
		return;
	}
	class_name = dex_class_name (bin, c);
	if (!class_name || !*class_name) {
		return;
	}
	const char *superClass = dex_class_super_name (bin, c);
	if (!superClass) {
		return;
	}
	class_name = strdup (class_name);
	r_str_replace_char (class_name, ';', 0);

	if (!class_name || !*class_name) {
		return;
	}
	RBinClass *cls = R_NEW0 (RBinClass);
	if (!cls) {
		free (class_name);
		return;
	}
	cls->name = class_name;
	cls->index = class_index;
	cls->addr = bin->header.class_offset + class_index * DEX_CLASS_SIZE;
	cls->methods = r_list_new ();
	cls->super = strdup (superClass);
	if (!cls->methods) {
		free (cls);
		free (class_name);
		return;
	}
	cls->fields = r_list_new ();
	if (!cls->fields) {
		r_list_free (cls->methods);
		free (class_name);
		free (cls);
		return;
	}
	r_list_append (bin->classes_list, cls);
	if (dexdump) {
		rbin->cb_printf ("  Class descriptor  : '%s;'\n", class_name);
		rbin->cb_printf (
			"  Access flags      : 0x%04x (%s)\n", c->access_flags,
			createAccessFlagStr (c->access_flags, kAccessForClass));
		rbin->cb_printf ("  Superclass        : '%s'\n",
				 dex_class_super_name (bin, c));
		rbin->cb_printf ("  Interfaces        -\n");
	}

	if (c->interfaces_offset > 0 &&
	    bin->header.data_offset < c->interfaces_offset &&
	    c->interfaces_offset <
		    bin->header.data_offset + bin->header.data_size) {
		int left;
		p = r_buf_get_at (binfile->buf, c->interfaces_offset, &left);
		if (left < 4) {
			return;
		}
		int types_list_size = r_read_le32 (p);
		if (types_list_size < 0 || types_list_size >= bin->header.types_size ) {
			return;
		}
		for (z = 0; z < types_list_size; z++) {
			ut16 le16;
			ut32 off = c->interfaces_offset + 4 + (z * 2);
			r_buf_read_at (binfile->buf, off, (ut8*)&le16, sizeof (le16));
			int t = r_read_le16 (&le16);
			if (t > 0 && t < bin->header.types_size ) {
				int tid = bin->types[t].descriptor_id;
				if (dexdump) {
					rbin->cb_printf (
						"    #%d              : '%s'\n",
						z, getstr (bin, tid));
				}
			}
		}
	}
	// TODO: this is quite ugly
	if (!c || !c->class_data_offset) {
		if (dexdump) {
			rbin->cb_printf (
				"  Static fields     -\n  Instance fields   "
				"-\n  Direct methods    -\n  Virtual methods   "
				"-\n");
		}
	} else {
		// TODO: move to func, def or inline
		// class_data_offset => [class_offset, class_defs_off+class_defs_size*32]
		if (bin->header.class_offset > c->class_data_offset ||
		    c->class_data_offset <
			    bin->header.class_offset +
				    bin->header.class_size * DEX_CLASS_SIZE) {
			return;
		}

		p = r_buf_get_at (binfile->buf, c->class_data_offset, NULL);
		p_end = p + binfile->buf->length - c->class_data_offset;
		//XXX check for NULL!!
		c->class_data = (struct dex_class_data_item_t *)malloc (
			sizeof (struct dex_class_data_item_t));
		p = r_uleb128 (p, p_end - p, &c->class_data->static_fields_size);
		p = r_uleb128 (p, p_end - p, &c->class_data->instance_fields_size);
		p = r_uleb128 (p, p_end - p, &c->class_data->direct_methods_size);
		p = r_uleb128 (p, p_end - p, &c->class_data->virtual_methods_size);

		if (dexdump) {
			rbin->cb_printf ("  Static fields     -\n");
		}
		p = parse_dex_class_fields (
			binfile, bin, c, cls, p, p_end, sym_count,
			c->class_data->static_fields_size, true);

		if (dexdump) {
			rbin->cb_printf ("  Instance fields   -\n");
		}
		p = parse_dex_class_fields (
			binfile, bin, c, cls, p, p_end, sym_count,
			c->class_data->instance_fields_size, false);

		if (dexdump) {
			rbin->cb_printf ("  Direct methods    -\n");
		}
		p = parse_dex_class_method (
			binfile, bin, c, cls, p, p_end, sym_count,
			c->class_data->direct_methods_size, methods, true);

		if (dexdump) {
			rbin->cb_printf ("  Virtual methods   -\n");
		}
		parse_dex_class_method (
			binfile, bin, c, cls, p, p_end, sym_count,
			c->class_data->virtual_methods_size, methods, false);
	}

	if (dexdump) {
		char *source_file = getstr (bin, c->source_file);
		if (!source_file) {
			rbin->cb_printf (
				"  source_file_idx   : %d (unknown)\n\n",
				c->source_file);
		} else {
			rbin->cb_printf ("  source_file_idx   : %d (%s)\n\n",
					 c->source_file, source_file);
		}
	}
	// TODO:!!!!
	// FIX: FREE BEFORE ALLOCATE!!!
	//free (class_name);
}

static bool is_class_idx_in_code_classes(RBinDexObj *bin, int class_idx) {
	int i;
	for (i = 0; i < bin->header.class_size; i++) {
		if (class_idx == bin->classes[i].class_id) {
			return true;
		}
	}
	return false;
}

static int dex_loadcode(RBinFile *bf, RBinDexObj *bin) {
	struct r_bin_t *rbin = bf->rbin;
	int i;
	int *methods = NULL;
	int sym_count = 0;

	// doublecheck??
	if (!bin || bin->methods_list) {
		return false;
	}
	bin->version = r_bin_dex_get_version (bin);
	bin->code_from = UT64_MAX;
	bin->code_to = 0;
	bin->methods_list = r_list_newf ((RListFree)free);
	if (!bin->methods_list) {
		return false;
	}
	bin->imports_list = r_list_newf ((RListFree)free);
	if (!bin->imports_list) {
		r_list_free (bin->methods_list);
		return false;
	}
	bin->lines_list = r_list_newf ((RListFree)free);
	if (!bin->lines_list) {
		return false;
	}
	bin->classes_list = r_list_newf ((RListFree)r_bin_class_free);
	if (!bin->classes_list) {
		r_list_free (bin->methods_list);
		r_list_free (bin->lines_list);
		r_list_free (bin->imports_list);
		return false;
	}

	if (bin->header.method_size>bin->size) {
		bin->header.method_size = 0;
		return false;
	}

	/* WrapDown the header sizes to avoid huge allocations */
	bin->header.method_size = R_MIN (bin->header.method_size, bin->size);
	bin->header.class_size = R_MIN (bin->header.class_size, bin->size);
	bin->header.strings_size = R_MIN (bin->header.strings_size, bin->size);

	// TODO: is this posible after R_MIN ??
	if (bin->header.strings_size > bin->size) {
		eprintf ("Invalid strings size\n");
		return false;
	}

	if (bin->classes) {
		ut64 amount = sizeof (int) * bin->header.method_size;
		if (amount > UT32_MAX || amount < bin->header.method_size) {
			return false;
		}
		methods = calloc (1, amount + 1);
		for (i = 0; i < bin->header.class_size; i++) {
			struct dex_class_t *c = &bin->classes[i];
			if (dexdump) {
				rbin->cb_printf ("Class #%d            -\n", i);
			}
			parse_class (bf, bin, c, i, methods, &sym_count);
		}
	}
	if (methods) {
		int import_count = 0;
		int sym_count = bin->methods_list->length;

		for (i = 0; i < bin->header.method_size; i++) {
			int len = 0;
			if (methods[i]) {
				continue;
			}

			if (bin->methods[i].class_id >= bin->header.types_size) {
				continue;
			}

			if (is_class_idx_in_code_classes(bin, bin->methods[i].class_id)) {
				continue;
			}

			const char *className = getstr (bin, bin->types[bin->methods[i].class_id].descriptor_id);
			if (!className) {
				continue;
			}
			char *class_name = strdup (className);
			if (!class_name) {
				free (class_name);
				continue;
			}
			len = strlen (class_name);
			if (len < 1) {
				free (class_name);
				continue;
			}
			r_str_replace_char (class_name, ';', 0);
			char *method_name = dex_method_name (bin, i);
			char *signature = dex_method_signature (bin, i);
			if (method_name && *method_name) {
				RBinImport *imp = R_NEW0 (RBinImport);
				if (!imp) {
					free (methods);
					free (signature);
					free (class_name);
					return false;
				}
				imp->name  = r_str_newf ("%s.method.%s%s", class_name, method_name, signature);
				imp->type = r_str_const ("FUNC");
				imp->bind = r_str_const ("NONE");
				imp->ordinal = import_count++;
				r_list_append (bin->imports_list, imp);

				RBinSymbol *sym = R_NEW0 (RBinSymbol);
				if (!sym) {
					free (methods);
					free (signature);
					free (class_name);
					return false;
				}
				sym->name = r_str_newf ("imp.%s", imp->name);
				sym->type = r_str_const (R_BIN_TYPE_FUNC_STR);
				sym->bind = r_str_const ("NONE");
				//XXX so damn unsafe check buffer boundaries!!!!
				//XXX use r_buf API!!
				sym->paddr = sym->vaddr = bin->b->base + bin->header.method_offset + (sizeof (struct dex_method_t) * i) ;
				sym->ordinal = sym_count++;
				r_list_append (bin->methods_list, sym);
				sdb_num_set (mdb, sdb_fmt ("method.%d", i), sym->paddr, 0);

			}
			free (signature);
			free (class_name);
		}
		free (methods);
	}
	return true;
}

static RList* imports(RBinFile *bf) {
	RBinDexObj *bin = (RBinDexObj*) bf->o->bin_obj;
	if (!bin) {
		return NULL;
	}
	if (bin && bin->imports_list) {
		return bin->imports_list;
	}
	dex_loadcode (bf, bin);
	return bin->imports_list;
}

static RList *methods(RBinFile *bf) {
	if (!bf || !bf->o || !bf->o->bin_obj) {
		return NULL;
	}
	RBinDexObj *bin = (RBinDexObj*) bf->o->bin_obj;
	if (!bin->methods_list) {
		dex_loadcode (bf, bin);
	}
	return bin->methods_list;
}

static RList *classes(RBinFile *bf) {
	RBinDexObj *bin;
	if (!bf || !bf->o || !bf->o->bin_obj) {
		return NULL;
	}
	bin = (RBinDexObj*) bf->o->bin_obj;
	if (!bin->classes_list) {
		dex_loadcode (bf, bin);
	}
	return bin->classes_list;
}

static int already_entry(RList *entries, ut64 vaddr) {
	RBinAddr *e;
	RListIter *iter;
	r_list_foreach (entries, iter, e) {
		if (e->vaddr == vaddr) {
			return 1;
		}
	}
	return 0;
}

static RList *entries(RBinFile *bf) {
	RListIter *iter;
	RBinDexObj *bin;
	RBinSymbol *m;
	RBinAddr *ptr;
	RList *ret;

	if (!bf || !bf->o || !bf->o->bin_obj) {
		return NULL;
	}
	bin = (RBinDexObj*) bf->o->bin_obj;
	ret = r_list_newf ((RListFree)free);

	if (!bin->methods_list) {
		dex_loadcode (bf, bin);
	}

	// STEP 1. ".onCreate(Landroid/os/Bundle;)V"
	r_list_foreach (bin->methods_list, iter, m) {
		if (strlen (m->name) > 30 && m->bind &&
			(!strcmp (m->bind, R_BIN_BIND_LOCAL_STR) || !strcmp (m->bind, R_BIN_BIND_GLOBAL_STR)) &&
		    !strcmp (m->name + strlen (m->name) - 31,
			     ".onCreate(Landroid/os/Bundle;)V")) {
			if (!already_entry (ret, m->paddr)) {
				if ((ptr = R_NEW0 (RBinAddr))) {
					ptr->paddr = ptr->vaddr = m->paddr;
					r_list_append (ret, ptr);
				}
			}
		}
	}

	// STEP 2. ".main([Ljava/lang/String;)V"
	if (r_list_empty (ret)) {
		r_list_foreach (bin->methods_list, iter, m) {
			if (strlen (m->name) > 26 &&
			    !strcmp (m->name + strlen (m->name) - 27,
				     ".main([Ljava/lang/String;)V")) {
				if (!already_entry (ret, m->paddr)) {
					if ((ptr = R_NEW0 (RBinAddr))) {
						ptr->paddr = ptr->vaddr = m->paddr;
						r_list_append (ret, ptr);
					}
				}
			}
		}
	}

	// STEP 3. NOTHING FOUND POINT TO CODE_INIT
	if (r_list_empty (ret)) {
		if (!already_entry (ret, bin->code_from)) {
			ptr = R_NEW0 (RBinAddr);
			if (ptr) {
				ptr->paddr = ptr->vaddr = bin->code_from;
				r_list_append (ret, ptr);
			}
		}
	}
	return ret;
}

static ut64 offset_of_method_idx(RBinFile *bf, struct r_bin_dex_obj_t *dex, int idx) {
	// ut64 off = dex->header.method_offset + idx;
	return sdb_num_get (mdb, sdb_fmt ("method.%d", idx), 0);
}

// TODO: change all return type for all getoffset
static int getoffset(RBinFile *bf, int type, int idx) {
	struct r_bin_dex_obj_t *dex = bf->o->bin_obj;
	switch (type) {
	case 'm': // methods
		// TODO: ADD CHECK
		return offset_of_method_idx (bf, dex, idx);
	case 'o': // objects
		break;
	case 's': // strings
		if (dex->header.strings_size > idx) {
			if (dex->strings) return dex->strings[idx];
		}
		break;
	case 't': // type
		return dex_get_type_offset (bf, idx);
	case 'c': // class
		return dex_get_type_offset (bf, idx);
	}
	return -1;
}

static char *getname(RBinFile *bf, int type, int idx) {
	struct r_bin_dex_obj_t *dex = bf->o->bin_obj;
	switch (type) {
	case 'm': // methods
		return dex_method_fullname (dex, idx);
	case 'c': // classes
		return dex_class_name_byid (dex, idx);
	case 'f': // fields
		return dex_field_name (dex, idx);
	case 'p': // proto
		return dex_get_proto (dex, idx);
	}
	return NULL;
}

static RList *sections(RBinFile *bf) {
	struct r_bin_dex_obj_t *bin = bf->o->bin_obj;
	RList *ml = methods (bf);
	RBinSection *ptr = NULL;
	int ns, fsymsz = 0;
	RList *ret = NULL;
	RListIter *iter;
	RBinSymbol *m;
	int fsym = 0;

	r_list_foreach (ml, iter, m) {
		if (!fsym || m->paddr < fsym) {
			fsym = m->paddr;
		}
		ns = m->paddr + m->size;
		if (ns > bf->buf->length) {
			continue;
		}
		if (ns > fsymsz) {
			fsymsz = ns;
		}
	}
	if (!fsym) {
		return NULL;
	}
	if (!(ret = r_list_new ())) {
		return NULL;
	}
	ret->free = free;

	if ((ptr = R_NEW0 (RBinSection))) {
		strcpy (ptr->name, "header");
		ptr->size = ptr->vsize = sizeof (struct dex_header_t);
		ptr->paddr= ptr->vaddr = 0;
		ptr->srwx = R_BIN_SCN_READABLE;
		ptr->add = true;
		r_list_append (ret, ptr);
	}
	if ((ptr = R_NEW0 (RBinSection))) {
		strcpy (ptr->name, "constpool");
		//ptr->size = ptr->vsize = fsym;
		ptr->paddr= ptr->vaddr = sizeof (struct dex_header_t);
		ptr->size = bin->code_from - ptr->vaddr; // fix size
		ptr->vsize = ptr->size;
		ptr->srwx = R_BIN_SCN_READABLE;
		ptr->add = true;
		r_list_append (ret, ptr);
	}
	if ((ptr = R_NEW0 (RBinSection))) {
		strcpy (ptr->name, "code");
		ptr->vaddr = ptr->paddr = bin->code_from; //ptr->vaddr = fsym;
		ptr->size = bin->code_to - ptr->paddr;
		ptr->vsize = ptr->size;
		ptr->srwx = R_BIN_SCN_READABLE | R_BIN_SCN_EXECUTABLE;
		ptr->add = true;
		r_list_append (ret, ptr);
	}
	if ((ptr = R_NEW0 (RBinSection))) {
		//ut64 sz = bf ? r_buf_size (bf->buf): 0;
		strcpy (ptr->name, "data");
		ptr->paddr = ptr->vaddr = fsymsz+fsym;
		if (ptr->vaddr > bf->buf->length) {
			ptr->paddr = ptr->vaddr = bin->code_to;
			ptr->size = ptr->vsize = bf->buf->length - ptr->vaddr;
		} else {
			ptr->size = ptr->vsize = bf->buf->length - ptr->vaddr;
			// hacky workaround
			//ptr->size = ptr->vsize = 1024;
		}
		ptr->srwx = R_BIN_SCN_READABLE; //|2;
		ptr->add = true;
		r_list_append (ret, ptr);
	}
	return ret;
}

static void header(RBinFile *bf) {
	struct r_bin_dex_obj_t *bin = bf->o->bin_obj;
	struct r_bin_t *rbin = bf->rbin;

	rbin->cb_printf ("DEX file header:\n");
	rbin->cb_printf ("magic               : 'dex\\n035\\0'\n");
	rbin->cb_printf ("checksum            : %x\n", bin->header.checksum);
	rbin->cb_printf ("signature           : %02x%02x...%02x%02x\n", bin->header.signature[0], bin->header.signature[1], bin->header.signature[18], bin->header.signature[19]);
	rbin->cb_printf ("file_size           : %d\n", bin->header.size);
	rbin->cb_printf ("header_size         : %d\n", bin->header.header_size);
	rbin->cb_printf ("link_size           : %d\n", bin->header.linksection_size);
	rbin->cb_printf ("link_off            : %d (0x%06x)\n", bin->header.linksection_offset, bin->header.linksection_offset);
	rbin->cb_printf ("string_ids_size     : %d\n", bin->header.strings_size);
	rbin->cb_printf ("string_ids_off      : %d (0x%06x)\n", bin->header.strings_offset, bin->header.strings_offset);
	rbin->cb_printf ("type_ids_size       : %d\n", bin->header.types_size);
	rbin->cb_printf ("type_ids_off        : %d (0x%06x)\n", bin->header.types_offset, bin->header.types_offset);
	rbin->cb_printf ("proto_ids_size       : %d\n", bin->header.prototypes_size);
	rbin->cb_printf ("proto_ids_off        : %d (0x%06x)\n", bin->header.prototypes_offset, bin->header.prototypes_offset);
	rbin->cb_printf ("field_ids_size      : %d\n", bin->header.fields_size);
	rbin->cb_printf ("field_ids_off       : %d (0x%06x)\n", bin->header.fields_offset, bin->header.fields_offset);
	rbin->cb_printf ("method_ids_size     : %d\n", bin->header.method_size);
	rbin->cb_printf ("method_ids_off      : %d (0x%06x)\n", bin->header.method_offset, bin->header.method_offset);
	rbin->cb_printf ("class_defs_size     : %d\n", bin->header.class_size);
	rbin->cb_printf ("class_defs_off      : %d (0x%06x)\n", bin->header.class_offset, bin->header.class_offset);
	rbin->cb_printf ("data_size           : %d\n", bin->header.data_size);
	rbin->cb_printf ("data_off            : %d (0x%06x)\n\n", bin->header.data_offset, bin->header.data_offset);

	// TODO: print information stored in the RBIN not this ugly fix
	dexdump = true;
	bin->methods_list = NULL;
	dex_loadcode (bf, bin);
	dexdump = false;
}

static ut64 size(RBinFile *bf) {
	int ret;
	ut32 off = 0, len = 0;
	ut8 u32s[sizeof (ut32)] = {0};

	ret = r_buf_read_at (bf->buf, 108, u32s, 4);
	if (ret != 4) {
		return 0;
	}
	off = r_read_le32 (u32s);
	ret = r_buf_read_at (bf->buf, 104, u32s, 4);
	if (ret != 4) {
		return 0;
	}
	len = r_read_le32 (u32s);
	return off + len;
}

static RList *lines(RBinFile *bf) {
	struct r_bin_dex_obj_t *dex = bf->o->bin_obj;
	/// XXX this is called more than once
	// r_sys_backtrace();
	return dex->lines_list;
	// return r_list_clone (dex->lines_list);
}

RBinPlugin r_bin_plugin_dex = {
	.name = "dex",
	.desc = "dex format bin plugin",
	.license = "LGPL3",
	.get_sdb = &get_sdb,
	.load = &load,
	.load_bytes = load_bytes,
	.load_buffer = &load_buffer,
	.check_bytes = check_bytes,
	.baddr = baddr,
	.entries = entries,
	.classes = classes,
	.sections = sections,
	.symbols = methods,
	.imports = imports,
	.strings = strings,
	.info = &info,
	.header = &header,
	.size = &size,
	.get_offset = &getoffset,
	.get_name = &getname,
	.dbginfo = &r_bin_dbginfo_dex,
	.lines = &lines,
};

#ifndef CORELIB
RLibStruct radare_plugin = {
	.type = R_LIB_TYPE_BIN,
	.data = &r_bin_plugin_dex,
	.version = R2_VERSION
};
#endif
