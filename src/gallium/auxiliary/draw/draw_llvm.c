#include "draw_llvm.h"

#include "draw_context.h"
#include "draw_vs.h"

#include "gallivm/lp_bld_arit.h"
#include "gallivm/lp_bld_interp.h"
#include "gallivm/lp_bld_struct.h"
#include "gallivm/lp_bld_type.h"
#include "gallivm/lp_bld_flow.h"
#include "gallivm/lp_bld_debug.h"
#include "gallivm/lp_bld_tgsi.h"
#include "gallivm/lp_bld_printf.h"
#include "gallivm/lp_bld_init.h"

#include "util/u_cpu_detect.h"
#include "util/u_string.h"

#include <llvm-c/Transforms/Scalar.h>

#define DEBUG_STORE 0


/* generates the draw jit function */
static void
draw_llvm_generate(struct draw_llvm *llvm, struct draw_llvm_variant *var);

static void
init_globals(struct draw_llvm *llvm)
{
   LLVMTypeRef texture_type;

   /* struct draw_jit_texture */
   {
      LLVMTypeRef elem_types[4];

      elem_types[DRAW_JIT_TEXTURE_WIDTH]  = LLVMInt32Type();
      elem_types[DRAW_JIT_TEXTURE_HEIGHT] = LLVMInt32Type();
      elem_types[DRAW_JIT_TEXTURE_STRIDE] = LLVMInt32Type();
      elem_types[DRAW_JIT_TEXTURE_DATA]   = LLVMPointerType(LLVMInt8Type(), 0);

      texture_type = LLVMStructType(elem_types, Elements(elem_types), 0);

      LP_CHECK_MEMBER_OFFSET(struct draw_jit_texture, width,
                             llvm->target, texture_type,
                             DRAW_JIT_TEXTURE_WIDTH);
      LP_CHECK_MEMBER_OFFSET(struct draw_jit_texture, height,
                             llvm->target, texture_type,
                             DRAW_JIT_TEXTURE_HEIGHT);
      LP_CHECK_MEMBER_OFFSET(struct draw_jit_texture, stride,
                             llvm->target, texture_type,
                             DRAW_JIT_TEXTURE_STRIDE);
      LP_CHECK_MEMBER_OFFSET(struct draw_jit_texture, data,
                             llvm->target, texture_type,
                             DRAW_JIT_TEXTURE_DATA);
      LP_CHECK_STRUCT_SIZE(struct draw_jit_texture,
                           llvm->target, texture_type);

      LLVMAddTypeName(llvm->module, "texture", texture_type);
   }


   /* struct draw_jit_context */
   {
      LLVMTypeRef elem_types[3];
      LLVMTypeRef context_type;

      elem_types[0] = LLVMPointerType(LLVMFloatType(), 0); /* vs_constants */
      elem_types[1] = LLVMPointerType(LLVMFloatType(), 0); /* vs_constants */
      elem_types[2] = LLVMArrayType(texture_type, PIPE_MAX_SAMPLERS); /* textures */

      context_type = LLVMStructType(elem_types, Elements(elem_types), 0);

      LP_CHECK_MEMBER_OFFSET(struct draw_jit_context, vs_constants,
                             llvm->target, context_type, 0);
      LP_CHECK_MEMBER_OFFSET(struct draw_jit_context, gs_constants,
                             llvm->target, context_type, 1);
      LP_CHECK_MEMBER_OFFSET(struct draw_jit_context, textures,
                             llvm->target, context_type,
                             DRAW_JIT_CONTEXT_TEXTURES_INDEX);
      LP_CHECK_STRUCT_SIZE(struct draw_jit_context,
                           llvm->target, context_type);

      LLVMAddTypeName(llvm->module, "draw_jit_context", context_type);

      llvm->context_ptr_type = LLVMPointerType(context_type, 0);
   }
   {
      LLVMTypeRef buffer_ptr = LLVMPointerType(LLVMIntType(8), 0);
      llvm->buffer_ptr_type = LLVMPointerType(buffer_ptr, 0);
   }
   /* struct pipe_vertex_buffer */
   {
      LLVMTypeRef elem_types[4];
      LLVMTypeRef vb_type;

      elem_types[0] = LLVMInt32Type();
      elem_types[1] = LLVMInt32Type();
      elem_types[2] = LLVMInt32Type();
      elem_types[3] = LLVMPointerType(LLVMOpaqueType(), 0); /* vs_constants */

      vb_type = LLVMStructType(elem_types, Elements(elem_types), 0);

      LP_CHECK_MEMBER_OFFSET(struct pipe_vertex_buffer, stride,
                             llvm->target, vb_type, 0);
      LP_CHECK_MEMBER_OFFSET(struct pipe_vertex_buffer, buffer_offset,
                             llvm->target, vb_type, 2);
      LP_CHECK_STRUCT_SIZE(struct pipe_vertex_buffer,
                           llvm->target, vb_type);

      LLVMAddTypeName(llvm->module, "pipe_vertex_buffer", vb_type);

      llvm->vb_ptr_type = LLVMPointerType(vb_type, 0);
   }
}

static LLVMTypeRef
create_vertex_header(struct draw_llvm *llvm, int data_elems)
{
   /* struct vertex_header */
   LLVMTypeRef elem_types[3];
   LLVMTypeRef vertex_header;
   char struct_name[24];

   util_snprintf(struct_name, 23, "vertex_header%d", data_elems);

   elem_types[0]  = LLVMIntType(32);
   elem_types[1]  = LLVMArrayType(LLVMFloatType(), 4);
   elem_types[2]  = LLVMArrayType(elem_types[1], data_elems);

   vertex_header = LLVMStructType(elem_types, Elements(elem_types), 0);

   /* these are bit-fields and we can't take address of them
      LP_CHECK_MEMBER_OFFSET(struct vertex_header, clipmask,
      llvm->target, vertex_header,
      DRAW_JIT_VERTEX_CLIPMASK);
      LP_CHECK_MEMBER_OFFSET(struct vertex_header, edgeflag,
      llvm->target, vertex_header,
      DRAW_JIT_VERTEX_EDGEFLAG);
      LP_CHECK_MEMBER_OFFSET(struct vertex_header, pad,
      llvm->target, vertex_header,
      DRAW_JIT_VERTEX_PAD);
      LP_CHECK_MEMBER_OFFSET(struct vertex_header, vertex_id,
      llvm->target, vertex_header,
      DRAW_JIT_VERTEX_VERTEX_ID);
   */
   LP_CHECK_MEMBER_OFFSET(struct vertex_header, clip,
                          llvm->target, vertex_header,
                          DRAW_JIT_VERTEX_CLIP);
   LP_CHECK_MEMBER_OFFSET(struct vertex_header, data,
                          llvm->target, vertex_header,
                          DRAW_JIT_VERTEX_DATA);

   LLVMAddTypeName(llvm->module, struct_name, vertex_header);

   return LLVMPointerType(vertex_header, 0);
}

struct draw_llvm *
draw_llvm_create(struct draw_context *draw)
{
   struct draw_llvm *llvm = CALLOC_STRUCT( draw_llvm );

   util_cpu_detect();

   llvm->draw = draw;
   llvm->engine = draw->engine;

   debug_assert(llvm->engine);

   llvm->module = LLVMModuleCreateWithName("draw_llvm");
   llvm->provider = LLVMCreateModuleProviderForExistingModule(llvm->module);

   LLVMAddModuleProvider(llvm->engine, llvm->provider);

   llvm->target = LLVMGetExecutionEngineTargetData(llvm->engine);

   llvm->pass = LLVMCreateFunctionPassManager(llvm->provider);
   LLVMAddTargetData(llvm->target, llvm->pass);
   /* These are the passes currently listed in llvm-c/Transforms/Scalar.h,
    * but there are more on SVN. */
   /* TODO: Add more passes */
   LLVMAddConstantPropagationPass(llvm->pass);
   if(util_cpu_caps.has_sse4_1) {
      /* FIXME: There is a bug in this pass, whereby the combination of fptosi
       * and sitofp (necessary for trunc/floor/ceil/round implementation)
       * somehow becomes invalid code.
       */
      LLVMAddInstructionCombiningPass(llvm->pass);
   }
   LLVMAddPromoteMemoryToRegisterPass(llvm->pass);
   LLVMAddGVNPass(llvm->pass);
   LLVMAddCFGSimplificationPass(llvm->pass);

   init_globals(llvm);


#if 1
   LLVMDumpModule(llvm->module);
#endif

   return llvm;
}

void
draw_llvm_destroy(struct draw_llvm *llvm)
{
   free(llvm);
}

struct draw_llvm_variant *
draw_llvm_prepare(struct draw_llvm *llvm, int num_inputs)
{
   struct draw_llvm_variant *variant = MALLOC(sizeof(struct draw_llvm_variant));

   draw_llvm_make_variant_key(llvm, &variant->key);

   llvm->vertex_header_ptr_type = create_vertex_header(llvm, num_inputs);

   draw_llvm_generate(llvm, variant);

   return variant;
}


struct draw_context *draw_create_with_llvm(void)
{
   struct draw_context *draw = CALLOC_STRUCT( draw_context );
   if (draw == NULL)
      goto fail;

   assert(lp_build_engine);
   draw->engine = lp_build_engine;

   if (!draw_init(draw))
      goto fail;

   return draw;

fail:
   draw_destroy( draw );
   return NULL;
}

static void
generate_vs(struct draw_llvm *llvm,
            LLVMBuilderRef builder,
            LLVMValueRef (*outputs)[NUM_CHANNELS],
            const LLVMValueRef (*inputs)[NUM_CHANNELS],
            LLVMValueRef context_ptr)
{
   const struct tgsi_token *tokens = llvm->draw->vs.vertex_shader->state.tokens;
   struct lp_type vs_type;
   LLVMValueRef consts_ptr = draw_jit_context_vs_constants(builder, context_ptr);

   memset(&vs_type, 0, sizeof vs_type);
   vs_type.floating = TRUE; /* floating point values */
   vs_type.sign = TRUE;     /* values are signed */
   vs_type.norm = FALSE;    /* values are not limited to [0,1] or [-1,1] */
   vs_type.width = 32;      /* 32-bit float */
   vs_type.length = 4;      /* 4 elements per vector */
#if 0
   num_vs = 4;              /* number of vertices per block */
#endif

   /*tgsi_dump(tokens, 0);*/
   lp_build_tgsi_soa(builder,
                     tokens,
                     vs_type,
                     NULL /*struct lp_build_mask_context *mask*/,
                     consts_ptr,
                     NULL /*pos*/,
                     inputs,
                     outputs,
                     NULL/*sampler*/);
}

#if DEBUG_STORE
static void print_vectorf(LLVMBuilderRef builder,
                         LLVMValueRef vec)
{
   LLVMValueRef val[4];
   val[0] = LLVMBuildExtractElement(builder, vec,
                                    LLVMConstInt(LLVMInt32Type(), 0, 0), "");
   val[1] = LLVMBuildExtractElement(builder, vec,
                                    LLVMConstInt(LLVMInt32Type(), 1, 0), "");
   val[2] = LLVMBuildExtractElement(builder, vec,
                                    LLVMConstInt(LLVMInt32Type(), 2, 0), "");
   val[3] = LLVMBuildExtractElement(builder, vec,
                                    LLVMConstInt(LLVMInt32Type(), 3, 0), "");
   lp_build_printf(builder, "vector = [%f, %f, %f, %f]\n",
                   val[0], val[1], val[2], val[3]);
}
#endif

static void
generate_fetch(LLVMBuilderRef builder,
               LLVMValueRef vbuffers_ptr,
               LLVMValueRef *res,
               struct pipe_vertex_element *velem,
               LLVMValueRef vbuf,
               LLVMValueRef index)
{
   LLVMValueRef indices = LLVMConstInt(LLVMInt64Type(), velem->vertex_buffer_index, 0);
   LLVMValueRef vbuffer_ptr = LLVMBuildGEP(builder, vbuffers_ptr,
                                           &indices, 1, "");
   LLVMValueRef vb_stride = draw_jit_vbuffer_stride(builder, vbuf);
   LLVMValueRef vb_buffer_offset = draw_jit_vbuffer_offset(builder, vbuf);
   LLVMValueRef stride = LLVMBuildMul(builder,
                                      vb_stride,
                                      index, "");

   vbuffer_ptr = LLVMBuildLoad(builder, vbuffer_ptr, "vbuffer");

   stride = LLVMBuildAdd(builder, stride,
                         vb_buffer_offset,
                         "");
   stride = LLVMBuildAdd(builder, stride,
                         LLVMConstInt(LLVMInt32Type(), velem->src_offset, 0),
                         "");

   /*lp_build_printf(builder, "vbuf index = %d, stride is %d\n", indices, stride);*/
   vbuffer_ptr = LLVMBuildGEP(builder, vbuffer_ptr, &stride, 1, "");

   *res = draw_llvm_translate_from(builder, vbuffer_ptr, velem->src_format);
}

static LLVMValueRef
aos_to_soa(LLVMBuilderRef builder,
           LLVMValueRef val0,
           LLVMValueRef val1,
           LLVMValueRef val2,
           LLVMValueRef val3,
           LLVMValueRef channel)
{
   LLVMValueRef ex, res;

   ex = LLVMBuildExtractElement(builder, val0,
                                channel, "");
   res = LLVMBuildInsertElement(builder,
                                LLVMConstNull(LLVMTypeOf(val0)),
                                ex,
                                LLVMConstInt(LLVMInt32Type(), 0, 0),
                                "");

   ex = LLVMBuildExtractElement(builder, val1,
                                channel, "");
   res = LLVMBuildInsertElement(builder,
                                res, ex,
                                LLVMConstInt(LLVMInt32Type(), 1, 0),
                                "");

   ex = LLVMBuildExtractElement(builder, val2,
                                channel, "");
   res = LLVMBuildInsertElement(builder,
                                res, ex,
                                LLVMConstInt(LLVMInt32Type(), 2, 0),
                                "");

   ex = LLVMBuildExtractElement(builder, val3,
                                channel, "");
   res = LLVMBuildInsertElement(builder,
                                res, ex,
                                LLVMConstInt(LLVMInt32Type(), 3, 0),
                                "");

   return res;
}

static void
soa_to_aos(LLVMBuilderRef builder,
           LLVMValueRef soa[NUM_CHANNELS],
           LLVMValueRef aos[NUM_CHANNELS])
{
   LLVMValueRef comp;
   int i = 0;

   debug_assert(NUM_CHANNELS == 4);

   aos[0] = LLVMConstNull(LLVMTypeOf(soa[0]));
   aos[1] = aos[2] = aos[3] = aos[0];

   for (i = 0; i < NUM_CHANNELS; ++i) {
      LLVMValueRef channel = LLVMConstInt(LLVMInt32Type(), i, 0);

      comp = LLVMBuildExtractElement(builder, soa[i],
                                     LLVMConstInt(LLVMInt32Type(), 0, 0), "");
      aos[0] = LLVMBuildInsertElement(builder, aos[0], comp, channel, "");

      comp = LLVMBuildExtractElement(builder, soa[i],
                                     LLVMConstInt(LLVMInt32Type(), 1, 0), "");
      aos[1] = LLVMBuildInsertElement(builder, aos[1], comp, channel, "");

      comp = LLVMBuildExtractElement(builder, soa[i],
                                     LLVMConstInt(LLVMInt32Type(), 2, 0), "");
      aos[2] = LLVMBuildInsertElement(builder, aos[2], comp, channel, "");

      comp = LLVMBuildExtractElement(builder, soa[i],
                                     LLVMConstInt(LLVMInt32Type(), 3, 0), "");
      aos[3] = LLVMBuildInsertElement(builder, aos[3], comp, channel, "");

   }
}

static void
convert_to_soa(LLVMBuilderRef builder,
               LLVMValueRef (*aos)[NUM_CHANNELS],
               LLVMValueRef (*soa)[NUM_CHANNELS],
               int num_attribs)
{
   int i;

   debug_assert(NUM_CHANNELS == 4);

   for (i = 0; i < num_attribs; ++i) {
      LLVMValueRef val0 = aos[i][0];
      LLVMValueRef val1 = aos[i][1];
      LLVMValueRef val2 = aos[i][2];
      LLVMValueRef val3 = aos[i][3];

      soa[i][0] = aos_to_soa(builder, val0, val1, val2, val3,
                             LLVMConstInt(LLVMInt32Type(), 0, 0));
      soa[i][1] = aos_to_soa(builder, val0, val1, val2, val3,
                             LLVMConstInt(LLVMInt32Type(), 1, 0));
      soa[i][2] = aos_to_soa(builder, val0, val1, val2, val3,
                             LLVMConstInt(LLVMInt32Type(), 2, 0));
      soa[i][3] = aos_to_soa(builder, val0, val1, val2, val3,
                             LLVMConstInt(LLVMInt32Type(), 3, 0));
   }
}

static void
store_aos(LLVMBuilderRef builder,
          LLVMValueRef io_ptr,
          LLVMValueRef index,
          LLVMValueRef value)
{
   LLVMValueRef id_ptr = draw_jit_header_id(builder, io_ptr);
   LLVMValueRef data_ptr = draw_jit_header_data(builder, io_ptr);
   LLVMValueRef indices[3];

   indices[0] = LLVMConstInt(LLVMInt32Type(), 0, 0);
   indices[1] = index;
   indices[2] = LLVMConstInt(LLVMInt32Type(), 0, 0);

   /* undefined vertex */
   LLVMBuildStore(builder, LLVMConstInt(LLVMInt32Type(),
                                        0xffff, 0), id_ptr);

#if DEBUG_STORE
   lp_build_printf(builder, "    ---- %p storing attribute %d (io = %p)\n", data_ptr, index, io_ptr);
#endif
#if 0
   /*lp_build_printf(builder, " ---- %p storing at %d (%p)  ", io_ptr, index, data_ptr);
     print_vectorf(builder, value);*/
   data_ptr = LLVMBuildBitCast(builder, data_ptr,
                               LLVMPointerType(LLVMArrayType(LLVMVectorType(LLVMFloatType(), 4), 0), 0),
                               "datavec");
   data_ptr = LLVMBuildGEP(builder, data_ptr, indices, 2, "");

   LLVMBuildStore(builder, value, data_ptr);
#else
   {
      LLVMValueRef x, y, z, w;
      LLVMValueRef idx0, idx1, idx2, idx3;
      LLVMValueRef gep0, gep1, gep2, gep3;
      data_ptr = LLVMBuildGEP(builder, data_ptr, indices, 3, "");

      idx0 = LLVMConstInt(LLVMInt32Type(), 0, 0);
      idx1 = LLVMConstInt(LLVMInt32Type(), 1, 0);
      idx2 = LLVMConstInt(LLVMInt32Type(), 2, 0);
      idx3 = LLVMConstInt(LLVMInt32Type(), 3, 0);

      x = LLVMBuildExtractElement(builder, value,
                                  idx0, "");
      y = LLVMBuildExtractElement(builder, value,
                                  idx1, "");
      z = LLVMBuildExtractElement(builder, value,
                                  idx2, "");
      w = LLVMBuildExtractElement(builder, value,
                                  idx3, "");

      gep0 = LLVMBuildGEP(builder, data_ptr, &idx0, 1, "");
      gep1 = LLVMBuildGEP(builder, data_ptr, &idx1, 1, "");
      gep2 = LLVMBuildGEP(builder, data_ptr, &idx2, 1, "");
      gep3 = LLVMBuildGEP(builder, data_ptr, &idx3, 1, "");

      /*lp_build_printf(builder, "##### x = %f (%p), y = %f (%p), z = %f (%p), w = %f (%p)\n",
        x, gep0, y, gep1, z, gep2, w, gep3);*/
      LLVMBuildStore(builder, x, gep0);
      LLVMBuildStore(builder, y, gep1);
      LLVMBuildStore(builder, z, gep2);
      LLVMBuildStore(builder, w, gep3);
   }
#endif
}

static void
store_aos_array(LLVMBuilderRef builder,
                LLVMValueRef io_ptr,
                LLVMValueRef aos[NUM_CHANNELS],
                int attrib,
                int num_outputs)
{
   LLVMValueRef attr_index = LLVMConstInt(LLVMInt32Type(), attrib, 0);
   LLVMValueRef ind0 = LLVMConstInt(LLVMInt32Type(), 0, 0);
   LLVMValueRef ind1 = LLVMConstInt(LLVMInt32Type(), 1, 0);
   LLVMValueRef ind2 = LLVMConstInt(LLVMInt32Type(), 2, 0);
   LLVMValueRef ind3 = LLVMConstInt(LLVMInt32Type(), 3, 0);
   LLVMValueRef io0_ptr, io1_ptr, io2_ptr, io3_ptr;

   debug_assert(NUM_CHANNELS == 4);

   io0_ptr = LLVMBuildGEP(builder, io_ptr,
                          &ind0, 1, "");
   io1_ptr = LLVMBuildGEP(builder, io_ptr,
                          &ind1, 1, "");
   io2_ptr = LLVMBuildGEP(builder, io_ptr,
                          &ind2, 1, "");
   io3_ptr = LLVMBuildGEP(builder, io_ptr,
                          &ind3, 1, "");

#if DEBUG_STORE
   lp_build_printf(builder, "   io = %p, indexes[%d, %d, %d, %d]\n",
                   io_ptr, ind0, ind1, ind2, ind3);
#endif

   store_aos(builder, io0_ptr, attr_index, aos[0]);
   store_aos(builder, io1_ptr, attr_index, aos[1]);
   store_aos(builder, io2_ptr, attr_index, aos[2]);
   store_aos(builder, io3_ptr, attr_index, aos[3]);
}

static void
convert_to_aos(LLVMBuilderRef builder,
               LLVMValueRef io,
               LLVMValueRef (*outputs)[NUM_CHANNELS],
               int num_outputs,
               int max_vertices)
{
   unsigned chan, attrib;

#if DEBUG_STORE
   lp_build_printf(builder, "   # storing begin\n");
#endif
   for (attrib = 0; attrib < num_outputs; ++attrib) {
      LLVMValueRef soa[4];
      LLVMValueRef aos[4];
      for(chan = 0; chan < NUM_CHANNELS; ++chan) {
         if(outputs[attrib][chan]) {
            LLVMValueRef out = LLVMBuildLoad(builder, outputs[attrib][chan], "");
            lp_build_name(out, "output%u.%c", attrib, "xyzw"[chan]);
            /*lp_build_printf(builder, "output %d : %d ",
                            LLVMConstInt(LLVMInt32Type(), attrib, 0),
                            LLVMConstInt(LLVMInt32Type(), chan, 0));
              print_vectorf(builder, out);*/
            soa[chan] = out;
         } else
            soa[chan] = 0;
      }
      soa_to_aos(builder, soa, aos);
      store_aos_array(builder,
                      io,
                      aos,
                      attrib,
                      num_outputs);
   }
#if DEBUG_STORE
   lp_build_printf(builder, "   # storing end\n");
#endif
}

static void
draw_llvm_generate(struct draw_llvm *llvm, struct draw_llvm_variant *variant)
{
   LLVMTypeRef arg_types[7];
   LLVMTypeRef func_type;
   LLVMValueRef context_ptr;
   LLVMBasicBlockRef block;
   LLVMBuilderRef builder;
   LLVMValueRef start, end, count, stride, step, io_itr;
   LLVMValueRef io_ptr, vbuffers_ptr, vb_ptr;
   struct draw_context *draw = llvm->draw;
   unsigned i, j;
   struct lp_build_context bld;
   struct lp_build_loop_state lp_loop;
   struct lp_type vs_type = lp_type_float_vec(32);
   const int max_vertices = 4;
   LLVMValueRef outputs[PIPE_MAX_SHADER_OUTPUTS][NUM_CHANNELS];

   arg_types[0] = llvm->context_ptr_type;           /* context */
   arg_types[1] = llvm->vertex_header_ptr_type;     /* vertex_header */
   arg_types[2] = llvm->buffer_ptr_type;            /* vbuffers */
   arg_types[3] = LLVMInt32Type();                  /* start */
   arg_types[4] = LLVMInt32Type();                  /* count */
   arg_types[5] = LLVMInt32Type();                  /* stride */
   arg_types[6] = llvm->vb_ptr_type;                /* pipe_vertex_buffer's */

   func_type = LLVMFunctionType(LLVMVoidType(), arg_types, Elements(arg_types), 0);

   variant->function = LLVMAddFunction(llvm->module, "draw_llvm_shader", func_type);
   LLVMSetFunctionCallConv(variant->function, LLVMCCallConv);
   for(i = 0; i < Elements(arg_types); ++i)
      if(LLVMGetTypeKind(arg_types[i]) == LLVMPointerTypeKind)
         LLVMAddAttribute(LLVMGetParam(variant->function, i), LLVMNoAliasAttribute);

   context_ptr  = LLVMGetParam(variant->function, 0);
   io_ptr       = LLVMGetParam(variant->function, 1);
   vbuffers_ptr = LLVMGetParam(variant->function, 2);
   start        = LLVMGetParam(variant->function, 3);
   count        = LLVMGetParam(variant->function, 4);
   stride       = LLVMGetParam(variant->function, 5);
   vb_ptr       = LLVMGetParam(variant->function, 6);

   lp_build_name(context_ptr, "context");
   lp_build_name(io_ptr, "io");
   lp_build_name(vbuffers_ptr, "vbuffers");
   lp_build_name(start, "start");
   lp_build_name(count, "count");
   lp_build_name(stride, "stride");
   lp_build_name(vb_ptr, "vb");

   /*
    * Function body
    */

   block = LLVMAppendBasicBlock(variant->function, "entry");
   builder = LLVMCreateBuilder();
   LLVMPositionBuilderAtEnd(builder, block);

   lp_build_context_init(&bld, builder, vs_type);

   end = lp_build_add(&bld, start, count);

   step = LLVMConstInt(LLVMInt32Type(), max_vertices, 0);

#if DEBUG_STORE
   lp_build_printf(builder, "start = %d, end = %d, step = %d\n",
                   start, end, step);
#endif
   lp_build_loop_begin(builder, start, &lp_loop);
   {
      LLVMValueRef inputs[PIPE_MAX_SHADER_INPUTS][NUM_CHANNELS];
      LLVMValueRef aos_attribs[PIPE_MAX_SHADER_INPUTS][NUM_CHANNELS];
      LLVMValueRef io;
      const LLVMValueRef (*ptr_aos)[NUM_CHANNELS];

      io_itr = LLVMBuildSub(builder, lp_loop.counter, start, "");
      io = LLVMBuildGEP(builder, io_ptr, &io_itr, 1, "");
#if DEBUG_STORE
      lp_build_printf(builder, " --- io %d = %p, loop counter %d\n",
                      io_itr, io, lp_loop.counter);
#endif
      for (i = 0; i < NUM_CHANNELS; ++i) {
         LLVMValueRef true_index = LLVMBuildAdd(
            builder,
            lp_loop.counter,
            LLVMConstInt(LLVMInt32Type(), i, 0), "");
         for (j = 0; j < draw->pt.nr_vertex_elements; ++j) {
            struct pipe_vertex_element *velem = &draw->pt.vertex_element[j];
            LLVMValueRef vb_index = LLVMConstInt(LLVMInt32Type(),
                                                 velem->vertex_buffer_index,
                                                 0);
            LLVMValueRef vb = LLVMBuildGEP(builder, vb_ptr,
                                           &vb_index, 1, "");
            generate_fetch(builder, vbuffers_ptr,
                           &aos_attribs[j][i], velem, vb, true_index);
         }
      }
      convert_to_soa(builder, aos_attribs, inputs,
                     draw->pt.nr_vertex_elements);

      ptr_aos = (const LLVMValueRef (*)[NUM_CHANNELS]) inputs;
      generate_vs(llvm,
                  builder,
                  outputs,
                  ptr_aos,
                  context_ptr);

      convert_to_aos(builder, io, outputs,
                     draw->vs.vertex_shader->info.num_outputs,
                     max_vertices);
   }
   lp_build_loop_end_cond(builder, end, step, LLVMIntUGE, &lp_loop);

   LLVMBuildRetVoid(builder);

   LLVMDisposeBuilder(builder);

   /*
    * Translate the LLVM IR into machine code.
    */
#ifdef DEBUG
   if(LLVMVerifyFunction(variant->function, LLVMPrintMessageAction)) {
      LLVMDumpValue(variant->function);
      assert(0);
   }
#endif

   LLVMRunFunctionPassManager(llvm->pass, variant->function);

   if (0) {
      LLVMDumpValue(variant->function);
      debug_printf("\n");
   }
   variant->jit_func = (draw_jit_vert_func)LLVMGetPointerToGlobal(llvm->draw->engine, variant->function);

   if (0)
      lp_disassemble(variant->jit_func);
}

void
draw_llvm_make_variant_key(struct draw_llvm *llvm,
                           struct draw_llvm_variant_key *key)
{
   memset(key, 0, sizeof(struct draw_llvm_variant_key));

   key->nr_vertex_elements = llvm->draw->pt.nr_vertex_elements;

   memcpy(key->vertex_element,
          llvm->draw->pt.vertex_element,
          sizeof(struct pipe_vertex_element) * key->nr_vertex_elements);

   memcpy(&key->vs,
          &llvm->draw->vs.vertex_shader->state,
          sizeof(struct pipe_shader_state));
}