#include "drake/geometry/optimization/dev/collision_geometry.h"

#include <fmt/format.h>
#include <gtest/gtest.h>

#include "drake/common/test_utilities/symbolic_test_util.h"
#include "drake/geometry/optimization/dev/test/c_iris_test_utilities.h"
#include "drake/geometry/optimization/vpolytope.h"
#include "drake/multibody/rational/rational_forward_kinematics.h"

namespace drake {
namespace geometry {
namespace optimization {

void SetupPlane(const Eigen::Ref<const VectorX<symbolic::Variable>>& s,
                Vector3<symbolic::Polynomial>* a, symbolic::Polynomial* b) {
  // Set each entry in a and b as an affine polynomial of s.
  Matrix3X<symbolic::Variable> a_coeff(3, s.rows());
  Vector3<symbolic::Variable> a_constant;
  const symbolic::Variables s_set(s);
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < s.rows(); ++j) {
      a_coeff(i, j) = symbolic::Variable(fmt::format("a_coeff({}, {})", i, j));
    }
    a_constant(i) = symbolic::Variable(fmt::format("a_constant({})", i));
    (*a)(i) = symbolic::Polynomial(
        a_coeff.row(i).dot(s.cast<symbolic::Expression>()) + a_constant(i),
        s_set);
  }
  VectorX<symbolic::Variable> b_coeff(s.rows());
  const symbolic::Variable b_constant("b_constant");
  for (int i = 0; i < s.rows(); ++i) {
    b_coeff(i) = symbolic::Variable(fmt::format("b_coeff({})", i));
  }
  *b = symbolic::Polynomial(
      b_coeff.dot(s.cast<symbolic::Expression>()) + b_constant, s_set);
}

TEST_F(CIrisToyRobotTest, BoxCollisionGeometry) {
  // Test CollisionGeometry constructed from a box.
  const auto& model_inspector = scene_graph_->model_inspector();
  CollisionGeometry box(&model_inspector.GetShape(body0_box_), body_indices_[0],
                        body0_box_, model_inspector.GetPoseInFrame(body0_box_));

  multibody::RationalForwardKinematics rational_forward_kin(plant_);
  Vector3<symbolic::Polynomial> a;
  symbolic::Polynomial b;
  SetupPlane(rational_forward_kin.s(), &a, &b);
  Eigen::Vector3d q_star(0., 0., 0.);
  const multibody::BodyIndex expressed_body = body_indices_[1];
  const auto X_AB_multilinear =
      rational_forward_kin.CalcBodyPoseAsMultilinearPolynomial(
          q_star, body_indices_[0], expressed_body);

  std::vector<symbolic::RationalFunction> rationals;
  std::optional<VectorX<symbolic::Polynomial>> unit_length_vector;
  // Positive side, separating_margin is empty.
  box.OnPlaneSide(a, b, X_AB_multilinear, rational_forward_kin, std::nullopt,
                  PlaneSide::kPositive, &rationals, &unit_length_vector);
  EXPECT_FALSE(unit_length_vector.has_value());
  EXPECT_EQ(rationals.size(), 8);

  // The order of the vertices should be the same in collision_geometry.cc
  Eigen::Matrix<double, 3, 8> p_GV;
  // clang-format off
  p_GV << 1, 1, 1, 1, -1, -1, -1, -1,
          1, 1, -1, -1, 1, 1, -1, -1,
          1, -1, 1, -1, 1, -1, 1, -1;
  // clang-format on
  auto box_geometry = static_cast<const Box&>(box.geometry());
  p_GV.row(0) *= box_geometry.width() / 2;
  p_GV.row(1) *= box_geometry.depth() / 2;
  p_GV.row(2) *= box_geometry.height() / 2;

  const Eigen::Matrix<double, 3, 8> p_BV = box.X_BG() * p_GV;

  // Evaluate the rationals, and compare the evaluation result with a.dot(p_AV)
  // + b - 1
  Eigen::Vector3d q_val(0.2, -0.1, 0.5);
  ASSERT_TRUE(
      (q_val.array() <= plant_->GetPositionUpperLimits().array()).all());
  ASSERT_TRUE(
      (q_val.array() >= plant_->GetPositionLowerLimits().array()).all());
  const Eigen::VectorXd s_val =
      rational_forward_kin.ComputeSValue(q_val, q_star);

  symbolic::Environment env;
  env.insert(rational_forward_kin.s(), s_val);
  auto diagram_context = diagram_->CreateDefaultContext();
  auto& plant_context =
      diagram_->GetMutableSubsystemContext(*plant_, diagram_context.get());
  plant_->SetPositions(&plant_context, q_val);
  Eigen::Matrix<double, 3, 8> p_AV;
  plant_->CalcPointsPositions(
      plant_context, plant_->get_body(body_indices_[0]).body_frame(), p_BV,
      plant_->get_body(expressed_body).body_frame(), &p_AV);
  Vector3<symbolic::Expression> a_expr;
  for (int j = 0; j < 3; ++j) {
    a_expr(j) = a(j).EvaluatePartial(env).ToExpression();
  }
  const symbolic::Expression b_expr = b.EvaluatePartial(env).ToExpression();
  for (int i = 0; i < 8; ++i) {
    const symbolic::Expression expr_expected =
        a_expr.dot(p_AV.col(i)) + b_expr - 1;
    const symbolic::Expression expr =
        rationals[i].numerator().EvaluatePartial(env).ToExpression() /
        rationals[i].denominator().Evaluate(env);
    EXPECT_PRED3(symbolic::test::PolynomialEqual, symbolic::Polynomial(expr),
                 symbolic::Polynomial(expr_expected), 1E-7);
  }

  // Negative side, with separating margin.
  rationals.clear();
  symbolic::Variable separating_margin{"delta"};
  box.OnPlaneSide(a, b, X_AB_multilinear, rational_forward_kin,
                  separating_margin, PlaneSide::kNegative, &rationals,
                  &unit_length_vector);
  EXPECT_TRUE(unit_length_vector.has_value());
  EXPECT_EQ(unit_length_vector->rows(), 3);
  for (int i = 0; i < 3; ++i) {
    EXPECT_EQ((*unit_length_vector)(i), a(i));
  }
  EXPECT_EQ(rationals.size(), 8);
  for (int i = 0; i < 8; ++i) {
    const symbolic::Expression expr_expected =
        -separating_margin - a_expr.dot(p_AV.col(i)) - b_expr;
    const symbolic::Expression expr =
        rationals[i].numerator().EvaluatePartial(env).ToExpression() /
        rationals[i].denominator().Evaluate(env);
    EXPECT_PRED3(symbolic::test::PolynomialEqual, symbolic::Polynomial(expr),
                 symbolic::Polynomial(expr_expected), 1E-7);
    EXPECT_TRUE(rationals[i].numerator().decision_variables().include(
        separating_margin));
  }
}

TEST_F(CIrisToyRobotTest, ConvexCollisionGeometry) {
  // Test CollisionGeometry constructed from a convex object.
  const auto& model_inspector = scene_graph_->model_inspector();
  CollisionGeometry convex(&model_inspector.GetShape(body1_convex_),
                           body_indices_[1], body1_convex_,
                           model_inspector.GetPoseInFrame(body1_convex_));

  multibody::RationalForwardKinematics rational_forward_kin(plant_);
  Vector3<symbolic::Polynomial> a;
  symbolic::Polynomial b;
  SetupPlane(rational_forward_kin.s(), &a, &b);
  Eigen::Vector3d q_star(0., 0., 0.);
  const multibody::BodyIndex expressed_body = body_indices_[3];
  const auto X_AB_multilinear =
      rational_forward_kin.CalcBodyPoseAsMultilinearPolynomial(
          q_star, body_indices_[1], expressed_body);

  std::vector<symbolic::RationalFunction> rationals;
  std::optional<VectorX<symbolic::Polynomial>> unit_length_vector;

  auto diagram_context = diagram_->CreateDefaultContext();
  auto& scene_graph_context = diagram_->GetMutableSubsystemContext(
      *scene_graph_, diagram_context.get());
  auto query_object =
      scene_graph_->get_query_output_port().Eval<QueryObject<double>>(
          scene_graph_context);

  const VPolytope polytope(query_object, body1_convex_,
                           model_inspector.GetFrameId(body1_convex_));

  const Eigen::Vector3d q_val(0.2, -0.1, 0.4);
  ASSERT_TRUE(
      (q_val.array() <= plant_->GetPositionUpperLimits().array()).all());
  ASSERT_TRUE(
      (q_val.array() >= plant_->GetPositionLowerLimits().array()).all());
  const Eigen::VectorXd s_val =
      rational_forward_kin.ComputeSValue(q_val, q_star);

  // The polytope reference frame is already set to the body, so I don't need to
  // multiply X_BG() here.
  const Eigen::Matrix3Xd p_BV = polytope.vertices();
  auto& plant_context =
      diagram_->GetMutableSubsystemContext(*plant_, diagram_context.get());
  plant_->SetPositions(&plant_context, q_val);
  Eigen::Matrix3Xd p_AV(3, polytope.vertices().cols());
  plant_->CalcPointsPositions(
      plant_context, plant_->get_body(body_indices_[1]).body_frame(), p_BV,
      plant_->get_body(expressed_body).body_frame(), &p_AV);

  // negative side, no separating margin.
  convex.OnPlaneSide(a, b, X_AB_multilinear, rational_forward_kin, std::nullopt,
                     PlaneSide::kNegative, &rationals, &unit_length_vector);
  EXPECT_FALSE(unit_length_vector.has_value());
  EXPECT_EQ(rationals.size(), polytope.vertices().cols());
  symbolic::Environment env;
  env.insert(rational_forward_kin.s(), s_val);
  Vector3<symbolic::Expression> a_expr;
  for (int j = 0; j < 3; ++j) {
    a_expr(j) = a(j).EvaluatePartial(env).ToExpression();
  }
  const symbolic::Expression b_expr = b.EvaluatePartial(env).ToExpression();
  for (int i = 0; i < polytope.vertices().cols(); ++i) {
    const symbolic::Expression expr_expected =
        -1 - a_expr.dot(p_AV.col(i)) - b_expr;
    const symbolic::Expression expr =
        rationals[i].numerator().EvaluatePartial(env).ToExpression() /
        rationals[i].denominator().Evaluate(env);
    EXPECT_PRED3(symbolic::test::PolynomialEqual,
                 symbolic::Polynomial(expr_expected),
                 symbolic::Polynomial(expr), 1E-7);
  }

  // Positive side, with separating margin.
  const symbolic::Variable separating_margin("delta");
  // Note that here I didn't clear rationals, so that I can test the new
  // rationals are appended to the existing ones.
  convex.OnPlaneSide(a, b, X_AB_multilinear, rational_forward_kin,
                     separating_margin, PlaneSide::kPositive, &rationals,
                     &unit_length_vector);
  EXPECT_TRUE(unit_length_vector.has_value());
  EXPECT_EQ(unit_length_vector->rows(), 3);
  for (int i = 0; i < 3; ++i) {
    EXPECT_EQ((*unit_length_vector)(i), a(i));
  }
  // The new rationals are appended to the existing ones.
  EXPECT_EQ(rationals.size(), 2 * polytope.vertices().cols());
  for (int i = 0; i < polytope.vertices().cols(); ++i) {
    const symbolic::Expression expr_expected =
        a_expr.dot(p_AV.col(i)) + b_expr - separating_margin;
    const symbolic::Expression expr =
        rationals[i + polytope.vertices().cols()]
            .numerator()
            .EvaluatePartial(env)
            .ToExpression() /
        rationals[i + polytope.vertices().cols()].denominator().Evaluate(env);
    EXPECT_PRED3(symbolic::test::PolynomialEqual,
                 symbolic::Polynomial(expr_expected),
                 symbolic::Polynomial(expr), 1E-7);
    EXPECT_TRUE(rationals[i + polytope.vertices().cols()]
                    .numerator()
                    .decision_variables()
                    .include(separating_margin));
  }
}
}  // namespace optimization
}  // namespace geometry
}  // namespace drake