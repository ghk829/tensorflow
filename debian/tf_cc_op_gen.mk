
include debian/flags.mk

TF_CC_OP_GEN_SRCS := \
	tensorflow/cc/framework/cc_op_gen.cc \
	tensorflow/cc/framework/cc_op_gen_main.cc
TF_CC_OP_GEN_OBJS := $(addprefix $(BDIR), $(TF_CC_OP_GEN_SRCS:.cc=.o))

test: $(TF_CC_OP_GEN_OBJS)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(LIBS) \
		build/tensorflow/core/ops/array_ops.o \
		$(TF_CC_OP_GEN_OBJS) $(TF_CORE) -o test
