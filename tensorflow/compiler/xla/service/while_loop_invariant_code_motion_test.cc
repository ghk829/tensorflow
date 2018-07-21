/* Copyright 2017 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow/compiler/xla/service/while_loop_invariant_code_motion.h"

#include "tensorflow/compiler/xla/service/hlo_matchers.h"
#include "tensorflow/compiler/xla/test.h"
#include "tensorflow/compiler/xla/tests/hlo_verified_test_base.h"
#include "tensorflow/compiler/xla/tools/parser/hlo_parser.h"
#include "tensorflow/core/lib/core/status_test_util.h"

namespace xla {
namespace {

namespace op = xla::testing::opcode_matchers;

class WhileLoopInvariantCodeMotionTest : public HloVerifiedTestBase {
 public:
  // Makes a computation which has one parameter, of the given shape, and always
  // returns PRED[]{true}.  This is useful as a dummy loop condition.
  HloComputation* MakeAlwaysTrueComputation(const Shape& param_shape,
                                            HloModule* module);
};

static void FindOnlyWhileInstruction(HloComputation* computation,
                                     HloInstruction** while_instruction) {
  *while_instruction = nullptr;
  for (auto* instr : computation->instructions()) {
    if (instr->opcode() == HloOpcode::kWhile) {
      ASSERT_EQ(*while_instruction, nullptr);
      *while_instruction = instr;
    }
  }

  ASSERT_NE(*while_instruction, nullptr);
}

HloComputation* WhileLoopInvariantCodeMotionTest::MakeAlwaysTrueComputation(
    const Shape& param_shape, HloModule* module) {
  HloComputation::Builder builder(TestName() + ".always_true");
  builder.AddInstruction(
      HloInstruction::CreateParameter(0, param_shape, "param"));
  builder.AddInstruction(
      HloInstruction::CreateConstant(Literal::CreateR0<bool>(true)));
  return module->AddEmbeddedComputation(builder.Build());
}

TEST_F(WhileLoopInvariantCodeMotionTest, HoistOneInvariantOperation) {
  auto scalar_s32 = ShapeUtil::MakeShape(S32, {});
  Shape while_shape =
      ShapeUtil::MakeTupleShape({scalar_s32, scalar_s32, scalar_s32});

  HloComputation* while_body = [&]() {
    HloComputation::Builder builder(TestName() + ".while_body");
    HloInstruction* param = builder.AddInstruction(
        HloInstruction::CreateParameter(0, while_shape, "param"));
    HloInstruction* gte_0 = builder.AddInstruction(
        HloInstruction::CreateGetTupleElement(scalar_s32, param, 0));
    HloInstruction* gte_1 = builder.AddInstruction(
        HloInstruction::CreateGetTupleElement(scalar_s32, param, 1));
    HloInstruction* add_result =
        builder.AddInstruction(HloInstruction::CreateBinary(
            scalar_s32, HloOpcode::kAdd, gte_0, gte_1));
    builder.AddInstruction(
        HloInstruction::CreateTuple({gte_0, gte_1, add_result}));

    return module().AddEmbeddedComputation(builder.Build());
  }();

  HloComputation::Builder builder(TestName());
  auto* init_value = builder.AddInstruction(
      HloInstruction::CreateParameter(0, while_shape, "init_value"));
  builder.AddInstruction(HloInstruction::CreateWhile(
      while_shape, MakeAlwaysTrueComputation(while_shape, &module()),
      while_body, init_value));
  HloComputation* entry_computation =
      module().AddEntryComputation(builder.Build());
  TF_ASSERT_OK_AND_ASSIGN(bool simplified_loop,
                          WhileLoopInvariantCodeMotion{}.Run(&module()));
  EXPECT_TRUE(simplified_loop);

  HloInstruction* transformed_while;
  FindOnlyWhileInstruction(entry_computation, &transformed_while);

  EXPECT_THAT(entry_computation->instructions(), Contains(op::Add()));
  EXPECT_THAT(transformed_while->while_body()->instructions(),
              Each(Not(op::Add())));
}

TEST_F(WhileLoopInvariantCodeMotionTest, HoistInvariantOperationTree) {
  auto scalar_s32 = ShapeUtil::MakeShape(S32, {});
  Shape while_shape =
      ShapeUtil::MakeTupleShape({scalar_s32, scalar_s32, scalar_s32});

  HloComputation* while_body = [&]() {
    HloComputation::Builder builder(TestName() + ".while_body");
    HloInstruction* param = builder.AddInstruction(
        HloInstruction::CreateParameter(0, while_shape, "param"));
    HloInstruction* gte_0 = builder.AddInstruction(
        HloInstruction::CreateGetTupleElement(scalar_s32, param, 0));
    HloInstruction* gte_1 = builder.AddInstruction(
        HloInstruction::CreateGetTupleElement(scalar_s32, param, 1));
    HloInstruction* gte_2_loop_variant = builder.AddInstruction(
        HloInstruction::CreateGetTupleElement(scalar_s32, param, 2));

    HloInstruction* add_result =
        builder.AddInstruction(HloInstruction::CreateBinary(
            scalar_s32, HloOpcode::kAdd, gte_0, gte_1));
    HloInstruction* mul_result =
        builder.AddInstruction(HloInstruction::CreateBinary(
            scalar_s32, HloOpcode::kMultiply, add_result, gte_1));
    HloInstruction* negate_result =
        builder.AddInstruction(HloInstruction::CreateUnary(
            scalar_s32, HloOpcode::kNegate, mul_result));
    HloInstruction* constant = builder.AddInstruction(
        HloInstruction::CreateConstant(Literal::CreateR0<int32>(4)));
    HloInstruction* sub_result =
        builder.AddInstruction(HloInstruction::CreateBinary(
            scalar_s32, HloOpcode::kSubtract, negate_result, constant));
    HloInstruction* divide_result =
        builder.AddInstruction(HloInstruction::CreateBinary(
            scalar_s32, HloOpcode::kDivide, sub_result, gte_2_loop_variant));
    builder.AddInstruction(
        HloInstruction::CreateTuple({gte_0, gte_1, divide_result}));

    return module().AddEmbeddedComputation(builder.Build());
  }();

  HloComputation::Builder builder(TestName());
  auto* init_value = builder.AddInstruction(
      HloInstruction::CreateParameter(0, while_shape, "init_value"));
  builder.AddInstruction(HloInstruction::CreateWhile(
      while_shape, MakeAlwaysTrueComputation(while_shape, &module()),
      while_body, init_value));
  HloComputation* entry_computation =
      module().AddEntryComputation(builder.Build());
  TF_ASSERT_OK_AND_ASSIGN(bool simplified_loop,
                          WhileLoopInvariantCodeMotion{}.Run(&module()));
  EXPECT_TRUE(simplified_loop);

  HloInstruction* transformed_while;
  FindOnlyWhileInstruction(entry_computation, &transformed_while);

  EXPECT_THAT(entry_computation->instructions(),
              AllOf(Contains(op::Add()), Contains(op::Multiply()),
                    Contains(op::Negate()), Contains(op::Subtract()),
                    Contains(op::Constant()),

                    // The division had a loop varying operand so that better
                    // not be hoisted.
                    Not(Contains(op::Divide()))));

  EXPECT_THAT(transformed_while->while_body()->instructions(),
              Each(Not(AnyOf(op::Add(), op::Multiply(), op::Negate(),
                             op::Subtract(), op::Constant()))));

  EXPECT_THAT(transformed_while->while_body()->instructions(),
              Contains(op::Divide()));
}

TEST_F(WhileLoopInvariantCodeMotionTest,
       DontHoistTriviallyLoopVaryingComputation) {
  // Basic negative test: the add expression is not loop invariant.
  auto scalar_s32 = ShapeUtil::MakeShape(S32, {});
  Shape while_shape = ShapeUtil::MakeTupleShape({scalar_s32, scalar_s32});

  HloComputation* while_body = [&]() {
    HloComputation::Builder builder(TestName() + ".while_body");
    HloInstruction* param = builder.AddInstruction(
        HloInstruction::CreateParameter(0, while_shape, "param"));
    HloInstruction* gte_0 = builder.AddInstruction(
        HloInstruction::CreateGetTupleElement(scalar_s32, param, 0));
    HloInstruction* gte_1 = builder.AddInstruction(
        HloInstruction::CreateGetTupleElement(scalar_s32, param, 1));
    HloInstruction* add_result =
        builder.AddInstruction(HloInstruction::CreateBinary(
            scalar_s32, HloOpcode::kAdd, gte_0, gte_1));
    builder.AddInstruction(HloInstruction::CreateTuple({gte_0, add_result}));

    return module().AddEmbeddedComputation(builder.Build());
  }();

  HloComputation::Builder builder(TestName());
  auto* init_value = builder.AddInstruction(
      HloInstruction::CreateParameter(0, while_shape, "init_value"));
  auto* while_inst = builder.AddInstruction(HloInstruction::CreateWhile(
      while_shape, MakeAlwaysTrueComputation(while_shape, &module()),
      while_body, init_value));

  module().AddEntryComputation(builder.Build());

  TF_ASSERT_OK_AND_ASSIGN(bool simplified_loop,
                          WhileLoopInvariantCodeMotion{}.Run(&module()));
  EXPECT_FALSE(simplified_loop);

  EXPECT_THAT(while_inst->while_body()->instructions(), Contains(op::Add()));
}

TEST_F(WhileLoopInvariantCodeMotionTest,
       DontHoistLoopVaryingComputationWithAlternatingTuples) {
  auto scalar_s32 = ShapeUtil::MakeShape(S32, {});
  Shape while_shape =
      ShapeUtil::MakeTupleShape({scalar_s32, scalar_s32, scalar_s32});

  HloComputation* while_body = [&]() {
    HloComputation::Builder builder(TestName() + ".while_body");
    HloInstruction* param = builder.AddInstruction(
        HloInstruction::CreateParameter(0, while_shape, "param"));
    HloInstruction* gte_0 = builder.AddInstruction(
        HloInstruction::CreateGetTupleElement(scalar_s32, param, 0));
    HloInstruction* gte_1 = builder.AddInstruction(
        HloInstruction::CreateGetTupleElement(scalar_s32, param, 1));
    HloInstruction* add_result =
        builder.AddInstruction(HloInstruction::CreateBinary(
            scalar_s32, HloOpcode::kAdd, gte_0, gte_1));
    builder.AddInstruction(
        HloInstruction::CreateTuple({gte_1, gte_0, add_result}));

    return module().AddEmbeddedComputation(builder.Build());
  }();

  HloComputation::Builder builder(TestName());
  auto* init_value = builder.AddInstruction(
      HloInstruction::CreateParameter(0, while_shape, "init_value"));
  auto* while_inst = builder.AddInstruction(HloInstruction::CreateWhile(
      while_shape, MakeAlwaysTrueComputation(while_shape, &module()),
      while_body, init_value));

  module().AddEntryComputation(builder.Build());
  TF_ASSERT_OK_AND_ASSIGN(bool simplified_loop,
                          WhileLoopInvariantCodeMotion{}.Run(&module()));
  EXPECT_FALSE(simplified_loop);

  EXPECT_THAT(while_inst->while_body()->instructions(), Contains(op::Add()));
}

TEST_F(WhileLoopInvariantCodeMotionTest, DontHoistInstructionWithSideEffects) {
  auto scalar_s32 = ShapeUtil::MakeShape(S32, {});
  Shape while_shape = ShapeUtil::MakeTupleShape({scalar_s32, scalar_s32});

  HloComputation* while_body = [&]() {
    HloComputation::Builder builder(TestName() + ".while_body");
    HloInstruction* param = builder.AddInstruction(
        HloInstruction::CreateParameter(0, while_shape, "param"));
    HloInstruction* gte_0 = builder.AddInstruction(
        HloInstruction::CreateGetTupleElement(scalar_s32, param, 0));
    HloInstruction* gte_1 = builder.AddInstruction(
        HloInstruction::CreateGetTupleElement(scalar_s32, param, 1));
    builder.AddInstruction(
        HloInstruction::CreateOutfeed(scalar_s32, gte_0, ""));
    builder.AddInstruction(HloInstruction::CreateTuple({gte_0, gte_1}));

    return module().AddEmbeddedComputation(builder.Build());
  }();

  HloComputation::Builder builder(TestName());
  auto* init_value = builder.AddInstruction(
      HloInstruction::CreateParameter(0, while_shape, "init_value"));
  auto* while_inst = builder.AddInstruction(HloInstruction::CreateWhile(
      while_shape, MakeAlwaysTrueComputation(while_shape, &module()),
      while_body, init_value));

  module().AddEntryComputation(builder.Build());

  TF_ASSERT_OK_AND_ASSIGN(bool simplified_loop,
                          WhileLoopInvariantCodeMotion{}.Run(&module()));
  EXPECT_FALSE(simplified_loop);

  EXPECT_THAT(while_inst->while_body()->instructions(),
              Contains(op::Outfeed()));
}

TEST_F(WhileLoopInvariantCodeMotionTest, DontHoistBitcastAlone) {
  // The bitcast's user, an outfeed, can't be hoisted, so don't hoist the
  // bitcast either.
  auto scalar_s32 = ShapeUtil::MakeShape(S32, {});
  auto scalar_f32 = ShapeUtil::MakeShape(F32, {});
  Shape while_shape = ShapeUtil::MakeTupleShape({scalar_s32, scalar_s32});

  HloComputation* while_body = [&]() {
    HloComputation::Builder builder(TestName() + ".while_body");
    HloInstruction* param = builder.AddInstruction(
        HloInstruction::CreateParameter(0, while_shape, "param"));
    HloInstruction* gte_0 = builder.AddInstruction(
        HloInstruction::CreateGetTupleElement(scalar_s32, param, 0));
    HloInstruction* gte_1 = builder.AddInstruction(
        HloInstruction::CreateGetTupleElement(scalar_s32, param, 1));
    HloInstruction* bitcast_inst = builder.AddInstruction(
        HloInstruction::CreateUnary(scalar_f32, HloOpcode::kBitcast, gte_0));
    builder.AddInstruction(
        HloInstruction::CreateOutfeed(scalar_f32, bitcast_inst, ""));
    builder.AddInstruction(HloInstruction::CreateTuple({gte_0, gte_1}));

    return module().AddEmbeddedComputation(builder.Build());
  }();

  HloComputation::Builder builder(TestName());
  auto* init_value = builder.AddInstruction(
      HloInstruction::CreateParameter(0, while_shape, "init_value"));
  auto* while_inst = builder.AddInstruction(HloInstruction::CreateWhile(
      while_shape, MakeAlwaysTrueComputation(while_shape, &module()),
      while_body, init_value));

  module().AddEntryComputation(builder.Build());

  TF_ASSERT_OK_AND_ASSIGN(bool simplified_loop,
                          WhileLoopInvariantCodeMotion{}.Run(&module()));
  EXPECT_FALSE(simplified_loop);

  EXPECT_THAT(while_inst->while_body()->instructions(),
              Contains(op::Outfeed()));
  EXPECT_THAT(while_inst->while_body()->instructions(),
              Contains(op::Bitcast()));
}

TEST_F(WhileLoopInvariantCodeMotionTest, HoistBitcastIfNeeded) {
  // The bitcast's user can be hoisted, so hoist the bitcast too.
  auto scalar_s32 = ShapeUtil::MakeShape(S32, {});
  auto scalar_f32 = ShapeUtil::MakeShape(F32, {});
  Shape while_shape =
      ShapeUtil::MakeTupleShape({scalar_s32, scalar_f32, scalar_f32});

  HloComputation* while_body = [&]() {
    HloComputation::Builder builder(TestName() + ".while_body");
    HloInstruction* param = builder.AddInstruction(
        HloInstruction::CreateParameter(0, while_shape, "param"));
    HloInstruction* gte_0 = builder.AddInstruction(
        HloInstruction::CreateGetTupleElement(scalar_s32, param, 0));
    HloInstruction* gte_1 = builder.AddInstruction(
        HloInstruction::CreateGetTupleElement(scalar_f32, param, 1));
    HloInstruction* bitcast_inst = builder.AddInstruction(
        HloInstruction::CreateUnary(scalar_f32, HloOpcode::kBitcast, gte_0));
    HloInstruction* add_inst =
        builder.AddInstruction(HloInstruction::CreateBinary(
            scalar_f32, HloOpcode::kAdd, bitcast_inst, gte_1));
    builder.AddInstruction(
        HloInstruction::CreateTuple({gte_0, gte_1, add_inst}));

    return module().AddEmbeddedComputation(builder.Build());
  }();

  HloComputation::Builder builder(TestName());
  auto* init_value = builder.AddInstruction(
      HloInstruction::CreateParameter(0, while_shape, "init_value"));
  builder.AddInstruction(HloInstruction::CreateWhile(
      while_shape, MakeAlwaysTrueComputation(while_shape, &module()),
      while_body, init_value));

  HloComputation* entry_computation =
      module().AddEntryComputation(builder.Build());

  TF_ASSERT_OK_AND_ASSIGN(bool simplified_loop,
                          WhileLoopInvariantCodeMotion{}.Run(&module()));
  EXPECT_TRUE(simplified_loop);

  HloInstruction* transformed_while;
  FindOnlyWhileInstruction(entry_computation, &transformed_while);

  EXPECT_THAT(transformed_while->while_body()->instructions(),
              Each(Not(op::Add())));
  EXPECT_THAT(transformed_while->while_body()->instructions(),
              Each(Not(op::Bitcast())));
  EXPECT_THAT(entry_computation->instructions(), Contains(op::Add()));
  EXPECT_THAT(entry_computation->instructions(), Contains(op::Bitcast()));
}

TEST_F(WhileLoopInvariantCodeMotionTest, DontHoistControlDependencies) {
  auto scalar_s32 = ShapeUtil::MakeShape(S32, {});
  Shape while_shape =
      ShapeUtil::MakeTupleShape({scalar_s32, scalar_s32, scalar_s32});

  HloComputation* while_body;
  {
    HloComputation::Builder builder(TestName() + ".while_body");
    HloInstruction* param = builder.AddInstruction(
        HloInstruction::CreateParameter(0, while_shape, "param"));
    HloInstruction* gte_0 = builder.AddInstruction(
        HloInstruction::CreateGetTupleElement(scalar_s32, param, 0));
    HloInstruction* gte_1 = builder.AddInstruction(
        HloInstruction::CreateGetTupleElement(scalar_s32, param, 1));
    HloInstruction* add_result =
        builder.AddInstruction(HloInstruction::CreateBinary(
            scalar_s32, HloOpcode::kAdd, gte_0, gte_1));
    TF_ASSERT_OK(param->AddControlDependencyTo(add_result));
    builder.AddInstruction(
        HloInstruction::CreateTuple({gte_0, gte_1, add_result}));

    while_body = module().AddEmbeddedComputation(builder.Build());
  }

  HloComputation::Builder builder(TestName());
  auto* init_value = builder.AddInstruction(
      HloInstruction::CreateParameter(0, while_shape, "init_value"));
  builder.AddInstruction(HloInstruction::CreateWhile(
      while_shape, MakeAlwaysTrueComputation(while_shape, &module()),
      while_body, init_value));
  module().AddEntryComputation(builder.Build());
  TF_ASSERT_OK_AND_ASSIGN(bool simplified_loop,
                          WhileLoopInvariantCodeMotion{}.Run(&module()));
  EXPECT_FALSE(simplified_loop);
}

TEST_F(WhileLoopInvariantCodeMotionTest, BodyHasNonTupleRoot) {
  auto scalar_s32 = ShapeUtil::MakeShape(S32, {});
  Shape while_shape = ShapeUtil::MakeTupleShape({scalar_s32, scalar_s32});

  HloComputation* while_body = [&]() {
    HloComputation::Builder builder(TestName() + ".passthrough");
    HloInstruction* param = builder.AddInstruction(
        HloInstruction::CreateParameter(0, while_shape, "param"));
    HloComputation* result = module().AddEmbeddedComputation(builder.Build());

    result->AddInstruction(
        HloInstruction::CreateGetTupleElement(scalar_s32, param, 1));
    return result;
  }();

  HloComputation::Builder builder(TestName());
  auto* init_value = builder.AddInstruction(
      HloInstruction::CreateParameter(0, while_shape, "init_value"));
  builder.AddInstruction(HloInstruction::CreateWhile(
      while_shape, MakeAlwaysTrueComputation(while_shape, &module()),
      while_body, init_value));
  module().AddEntryComputation(builder.Build());
  TF_ASSERT_OK_AND_ASSIGN(bool simplified_loop,
                          WhileLoopInvariantCodeMotion{}.Run(&module()));
  EXPECT_FALSE(simplified_loop);
}

const char* const kConstantHoistingTestCase = R"(
HloModule ModuleWithWhile

body {
  p_body = (f32[2]{0}) parameter(0)
  p_body.1 = f32[2]{0} get-tuple-element(p_body), index=0
  const = f32[2]{0} constant({3, 4})
  add.0 = f32[2]{0} add(p_body.1, const)
  ROOT root = (f32[2]{0}) tuple(add.0)
}

condition {
  p_cond = (f32[2]{0}) parameter(0)
  ROOT result = pred[] constant(true)
}

ENTRY entry {
  const_0 = f32[2]{0} constant({1, 2})
  while_init = (f32[2]{0}) tuple(const_0)
  ROOT while = (f32[2]{0}) while(while_init), condition=condition, body=body
}
)";

TEST_F(WhileLoopInvariantCodeMotionTest, HoistsConstantWhenAsked) {
  ParseAndVerifyModule(kConstantHoistingTestCase);

  TF_ASSERT_OK_AND_ASSIGN(
      bool simplified_loop,
      WhileLoopInvariantCodeMotion{/*hoist_constants=*/true}.Run(&module()));
  EXPECT_TRUE(simplified_loop);

  HloComputation* while_body = module().GetComputationWithName("wide.body");
  ASSERT_NE(while_body, nullptr);

  // We expect the while body to be the equivalent of:
  //
  //  wide.body {
  //    wide_param.1 = (f32[2]{0}, f32[2]{0}) parameter(0)
  //    get-tuple-element.1 = f32[2]{0} get-tuple-element(wide_param.1), index=0
  //    tuple.1 = (f32[2]{0}) tuple(get-tuple-element.1)
  //    get-tuple-element.4 = f32[2]{0} get-tuple-element(tuple.1), index=0
  //    get-tuple-element.7 = f32[2]{0} get-tuple-element(wide_param.1), index=1
  //    add.1 = f32[2]{0} add(get-tuple-element.4, get-tuple-element.7)
  //    tuple.3 = (f32[2]{0}) tuple(add.1)
  //    get-tuple-element.8 = f32[2]{0} get-tuple-element(tuple.3), index=0
  //    get-tuple-element.9 = f32[2]{0} get-tuple-element(wide_param.1), index=1
  //    ROOT tuple.4 = (f32[2]{0}, f32[2]{0}) tuple(get-tuple-element.8,
  //                                                get-tuple-element.9)
  //  }

  auto wide_param_1 = op::Parameter(0);
  auto get_tuple_element_1 = op::GetTupleElement(wide_param_1, 0);
  auto tuple_1 = op::Tuple(get_tuple_element_1);
  auto get_tuple_element_4 = op::GetTupleElement(tuple_1, 0);
  auto get_tuple_element_7 = op::GetTupleElement(wide_param_1, 1);
  auto add_1 = op::Add(get_tuple_element_4, get_tuple_element_7);
  auto tuple_3 = op::Tuple(add_1);
  auto get_tuple_element_8 = op::GetTupleElement(tuple_3, 0);
  auto get_tuple_element_9 = op::GetTupleElement(wide_param_1, 1);
  auto tuple_4 = op::Tuple(get_tuple_element_8, get_tuple_element_9);

  EXPECT_THAT(while_body->root_instruction(), tuple_4);
}

TEST_F(WhileLoopInvariantCodeMotionTest, DoesNotHoistConstantByDefault) {
  ParseAndVerifyModule(kConstantHoistingTestCase);

  TF_ASSERT_OK_AND_ASSIGN(bool simplified_loop,
                          WhileLoopInvariantCodeMotion{}.Run(&module()));
  EXPECT_FALSE(simplified_loop);
}

}  // namespace
}  // namespace xla