#include "drake/multibody/plant/multibody_plant.h"

#include <algorithm>
#include <functional>
#include <limits>
#include <memory>
#include <set>
#include <stdexcept>
#include <vector>

#include "drake/common/drake_throw.h"
#include "drake/common/ssize.h"
#include "drake/common/text_logging.h"
#include "drake/common/unused.h"
#include "drake/geometry/geometry_frame.h"
#include "drake/geometry/geometry_instance.h"
#include "drake/geometry/geometry_roles.h"
#include "drake/geometry/proximity_properties.h"
#include "drake/geometry/query_results/contact_surface.h"
#include "drake/geometry/render/render_label.h"
#include "drake/math/random_rotation.h"
#include "drake/math/rotation_matrix.h"
#include "drake/multibody/hydroelastics/hydroelastic_engine.h"
#include "drake/multibody/plant/externally_applied_spatial_force.h"
#include "drake/multibody/plant/hydroelastic_traction_calculator.h"
#include "drake/multibody/plant/make_discrete_update_manager.h"
#include "drake/multibody/plant/slicing_and_indexing.h"
#include "drake/multibody/tree/prismatic_joint.h"
#include "drake/multibody/tree/quaternion_floating_joint.h"
#include "drake/multibody/tree/revolute_joint.h"

namespace drake {
namespace multibody {

// Helper macro to throw an exception within methods that should not be called
// post-finalize.
#define DRAKE_MBP_THROW_IF_FINALIZED() ThrowIfFinalized(__func__)

// Helper macro to throw an exception within methods that should not be called
// pre-finalize.
#define DRAKE_MBP_THROW_IF_NOT_FINALIZED() ThrowIfNotFinalized(__func__)

using geometry::CollisionFilterDeclaration;
using geometry::ContactSurface;
using geometry::FrameId;
using geometry::FramePoseVector;
using geometry::GeometryFrame;
using geometry::GeometryId;
using geometry::GeometryInstance;
using geometry::PenetrationAsPointPair;
using geometry::ProximityProperties;
using geometry::render::RenderLabel;
using geometry::SceneGraph;
using geometry::SourceId;
using systems::InputPort;
using systems::OutputPort;
using systems::State;

using drake::math::RigidTransform;
using drake::math::RotationMatrix;
using drake::multibody::internal::AccelerationKinematicsCache;
using drake::multibody::internal::ArticulatedBodyForceCache;
using drake::multibody::internal::ArticulatedBodyInertiaCache;
using drake::multibody::internal::PositionKinematicsCache;
using drake::multibody::internal::VelocityKinematicsCache;
using drake::multibody::MultibodyForces;
using drake::multibody::SpatialAcceleration;
using drake::multibody::SpatialForce;
using systems::BasicVector;
using systems::Context;
using systems::InputPort;
using systems::InputPortIndex;
using systems::OutputPortIndex;

namespace internal {
// This is a helper struct used to estimate the parameters used in the penalty
// method to enforce joint limits.
// The penalty method applies at each joint, a spring-damper force with
// parameters estimated by this struct.
// Once a joint reaches a limit (either lower or upper), the governing equations
// for that joint's coordinate can be approximated by a harmonic oscillator with
// stiffness and damping corresponding to the penalty parameters for that joint
// as:  q̈ + 2ζω₀ q̇ + ω₀² q = 0, where ω₀² = k / m̃ is the characteristic
// numerical stiffness frequency and m̃ is an inertia term computed differently
// for prismatic and revolute joints.
// The numerical frequency is defined as ω₀ = 2π/τ₀ with τ₀ = αδt a numerical
// stiffness time scale set to be proportional to the time step of the discrete
// model. The damping ratio ζ is set to one, corresponding to a critically
// damped oscillator and thus so that the penalty method emulates the effect of
// a "hard" limit.
// Knowing ω₀ (from the time step) and m̃ (a function of the bodies connected by
// the joint), it is possible, from the equations for a harmonic oscillator, to
// estimate the stiffness k and damping d parameters for the penalty method.
// Finally, MultibodyPlant uses a value of α to guarantee the stability of the
// method (from a stability analysis of the time stepping method for the
// model of a harmonic oscillator).
// Using this estimation procedure, the stiffness k is shown to be proportional
// to the inverse of the time step squared, i.e. k ∝ 1/δt².
// Since, at steady state, the violation of the joint limit is inversely
// proportional to the stiffness parameter, this violation turns out being
// proportional to the time step squared, that is, Δq ∝ δt².
// Therefore the convergence of the joint limit violation is expected to be
// quadratic with the time step.
template <typename T>
struct JointLimitsPenaltyParametersEstimator {
  // This helper method returns a pair (k, d) (in that order) for a harmonic
  // oscillator given the period τ₀ of the oscillator and the inertia m̃. d is
  // computed for a critically damped oscillator.
  // The harmonic oscillator model corresponds to:
  //    m̃q̈ + d q̇ + k q = 0
  // or equivalently:
  //    q̈ + 2ζω₀ q̇ + ω₀² q = 0
  // with ω₀ = sqrt(k/m̃) and ζ = d/sqrt(km̃)/2 the damping ratio, which is one
  // for critically damped oscillators.
  static std::pair<double, double>
  CalcCriticallyDampedHarmonicOscillatorParameters(
      double period, double inertia) {
    const double damping_ratio = 1.0;  // Critically damped.
    const double omega0 = 2.0 * M_PI / period;
    const double stiffness = inertia * omega0 * omega0;
    const double damping = 2.0 * damping_ratio * std::sqrt(inertia * stiffness);
    return std::make_pair(stiffness, damping);
  }

  // This method combines a pair of penalty parameters params1 and params2.
  // The combination law is very simple, this method returns the set of
  // parameters with the smallest stiffness, and thus it favors the stiffness
  // leading to the lower numerical stiffness (thus guaranteeing stability).
  static std::pair<double, double> PickLessStiffPenaltyParameters(
      const std::pair<double, double>& params1,
      const std::pair<double, double>& params2) {
    const double stiffness1 = params1.first;
    const double stiffness2 = params2.first;
    if (stiffness1 < stiffness2) {
      return params1;
    } else {
      return params2;
    }
  }

  // Helper method to estimate the penalty parameters for a prismatic joint.
  // The strategy consists in computing a set of penalty parameters for each
  // body connected by joint as if the other body was welded and ignoring
  // any other bodies in the system. This leads to a spring mass system where
  // the inertia m̃ corresponds to the mass of the body in consideration.
  // Then the penalty parameters estimated for each body are combined with
  // PickLessStiffPenaltyParameters() leading to a single set of parameters.
  static std::pair<double, double> CalcPrismaticJointPenaltyParameters(
      const PrismaticJoint<T>& joint, double numerical_time_scale) {
    // Penalty parameters for the parent body (child fixed).
    const double parent_mass = joint.parent_body().index() == world_index() ?
                               std::numeric_limits<double>::infinity() :
                               joint.parent_body().default_mass();
    const auto parent_params = CalcCriticallyDampedHarmonicOscillatorParameters(
        numerical_time_scale, parent_mass);
    // Penalty parameters for the child body (parent fixed).
    const double child_mass = joint.child_body().index() == world_index() ?
                               std::numeric_limits<double>::infinity() :
                               joint.child_body().default_mass();
    const auto child_params = CalcCriticallyDampedHarmonicOscillatorParameters(
        numerical_time_scale, child_mass);

    // Return the combined penalty parameters of the two bodies.
    auto params = PickLessStiffPenaltyParameters(parent_params, child_params);

    return params;
  }

  // Helper method to estimate the penalty parameters for a revolute joint.
  // The strategy consists in computing a set of penalty parameters for each
  // body connected by joint as if the other body was welded and ignoring
  // any other bodies in the system. This leads to a torsional spring system
  // for which the inertia m̃ corresponds to the rotational inertia of the body
  // in consideration, computed about the axis of the joint.
  // Then the penalty parameters estimated for each body are combined with
  // PickLessStiffPenaltyParameters() leading to a single set of parameters.
  static std::pair<double, double> CalcRevoluteJointPenaltyParameters(
      const RevoluteJoint<T>& joint, double numerical_time_scale) {
    // For the body attached to `frame` (one of the parent/child frames of
    // `joint`), this helper lambda computes the rotational inertia of the body
    // about the axis of the joint.
    // That is, it computes Iₐ = âᵀ⋅Iᴮ⋅â where Iᴮ is the rotational inertia of
    // the body, â is the axis of the joint, and Iₐ is the (scalar) rotational
    // inertia of the body computed about the joint's axis. Iₐ is the inertia
    // that must be considered for the problem of a pendulum oscillating about
    // an axis â, leading to the equations for a harmonic oscillator when we
    // apply the penalty forces.
    // For further details on Iₐ, the interested reader can refer to
    // [Goldstein, 2014, §5.3].
    //
    // [Goldstein, 2014] Goldstein, H., Poole, C.P. and Safko, J.L., 2014.
    //                   Classical Mechanics: Pearson New International Edition.
    //                   Pearson Higher Ed.
    auto CalcRotationalInertiaAboutAxis = [&joint](const Frame<T>& frame) {
          const RigidBody<T>* body =
              dynamic_cast<const RigidBody<T>*>(&frame.body());
          DRAKE_THROW_UNLESS(body != nullptr);

          // This check is needed for such models for which the user leaves the
          // spatial inertias unspecified (i.e. initialized to NaN). A user
          // might do this when only interested in performing kinematics
          // computations.
          if (std::isnan(body->default_mass())) {
            return std::numeric_limits<double>::infinity();
          }

          const SpatialInertia<T>& M_PPo_P =
              body->default_spatial_inertia().template cast<T>();
          const RigidTransform<T> X_PJ = frame.GetFixedPoseInBodyFrame();
          const Vector3<T>& p_PJ = X_PJ.translation();
          const math::RotationMatrix<T>& R_PJ = X_PJ.rotation();
          const SpatialInertia<T> M_PJo_J =
              M_PPo_P.Shift(p_PJ).ReExpress(R_PJ);
          const RotationalInertia<T> I_PJo_J =
              M_PJo_J.CalcRotationalInertia();
          // Rotational inertia about the joint axis.
          const Vector3<T>& axis = joint.revolute_axis();
          const T I_a = axis.transpose() * (I_PJo_J * axis);
          return ExtractDoubleOrThrow(I_a);
        };

    // Rotational inertia about the joint's axis for the parent body.
    const double I_Pa =
        joint.parent_body().index() == world_index() ?
        std::numeric_limits<double>::infinity() :
        CalcRotationalInertiaAboutAxis(joint.frame_on_parent());
    auto parent_params = CalcCriticallyDampedHarmonicOscillatorParameters(
        numerical_time_scale, I_Pa);

    // Rotational inertia about the joint's axis for the child body.
    const double I_Ca =
        joint.child_body().index() == world_index() ?
        std::numeric_limits<double>::infinity() :
        CalcRotationalInertiaAboutAxis(joint.frame_on_child());
    auto child_params = CalcCriticallyDampedHarmonicOscillatorParameters(
        numerical_time_scale, I_Ca);

    // Return the combined penalty parameters of the two bodies.
    return PickLessStiffPenaltyParameters(parent_params, child_params);
  }
};
}  // namespace internal

namespace {

// Hack to fully qualify frame names, pending resolution of #9128. Used by
// geometry registration routines. When this hack is removed, also undo the
// de-hacking step within internal_geometry_names.cc. Note that unlike the
// ScopedName convention, here the world and default model instances do not
// use any scoping.
template <typename T>
std::string GetScopedName(
    const MultibodyPlant<T>& plant,
    ModelInstanceIndex model_instance, const std::string& name) {
  if (model_instance != world_model_instance() &&
      model_instance != default_model_instance()) {
    return plant.GetModelInstanceName(model_instance) + "::" + name;
  } else {
    return name;
  }
}

}  // namespace

template <typename T>
MultibodyPlant<T>::MultibodyPlant(double time_step)
    : MultibodyPlant(nullptr, time_step) {
  // Cross-check that the Config default matches our header file default.
  DRAKE_DEMAND(contact_model_ == ContactModel::kHydroelasticWithFallback);
  DRAKE_DEMAND(MultibodyPlantConfig{}.contact_model ==
               "hydroelastic_with_fallback");
  DRAKE_DEMAND(contact_solver_enum_ == DiscreteContactSolver::kTamsi);
  DRAKE_DEMAND(MultibodyPlantConfig{}.discrete_contact_solver == "tamsi");
}

template <typename T>
MultibodyPlant<T>::MultibodyPlant(
    std::unique_ptr<internal::MultibodyTree<T>> tree_in, double time_step)
    : internal::MultibodyTreeSystem<T>(
          systems::SystemTypeTag<MultibodyPlant>{},
          std::move(tree_in), time_step > 0),
      contact_surface_representation_(
        GetDefaultContactSurfaceRepresentation(time_step)),
      time_step_(time_step) {
  DRAKE_THROW_UNLESS(time_step >= 0);
  // TODO(eric.cousineau): Combine all of these elements into one struct, make
  // it less brittle.
  visual_geometries_.emplace_back();  // Entries for the "world" body.
  collision_geometries_.emplace_back();
  // Add the world body to the graph.
  multibody_graph_.AddBody(world_body().name(), world_body().model_instance());
  DeclareSceneGraphPorts();
}

template <typename T>
template <typename U>
MultibodyPlant<T>::MultibodyPlant(const MultibodyPlant<U>& other)
    : internal::MultibodyTreeSystem<T>(
          systems::SystemTypeTag<MultibodyPlant>{},
          other.internal_tree().template CloneToScalar<T>(),
          other.is_discrete()) {
  DRAKE_THROW_UNLESS(other.is_finalized());

  // Here we step through every member field one by one, in the exact order
  // they are declared in the header, so that a reader could mindlessly compare
  // this function to the private fields, and check that every single field got
  // a mention.
  // For each field, this function will either:
  // (1) Copy the field directly.
  // (2) Place a forward-reference comment like "We initialize
  //     geometry_query_port_ during DeclareSceneGraphPorts, below."
  // (3) Place a disclaimer comment why that field does not need to be copied
  {
    source_id_ = other.source_id_;
    penalty_method_contact_parameters_ =
        other.penalty_method_contact_parameters_;
    penetration_allowance_ = other.penetration_allowance_;
    // Copy over the friction model if it is initialized. Otherwise, a default
    // value will be set in FinalizePlantOnly().
    // Note that stiction_tolerance is the only real data field in
    // `friction_model_`, so setting the stiction tolerance is equivalent to
    // copying `friction_model_`.
    if (other.friction_model_.stiction_tolerance() > 0) {
      friction_model_.set_stiction_tolerance(
          other.friction_model_.stiction_tolerance());
    }
    // joint_limit_parameters_ is set in SetUpJointLimitsParameters() in
    // FinalizePlantOnly().
    body_index_to_frame_id_ = other.body_index_to_frame_id_;
    frame_id_to_body_index_ = other.frame_id_to_body_index_;
    geometry_id_to_body_index_ = other.geometry_id_to_body_index_;
    visual_geometries_ = other.visual_geometries_;
    num_visual_geometries_ = other.num_visual_geometries_;
    collision_geometries_ = other.collision_geometries_;
    num_collision_geometries_ = other.num_collision_geometries_;
    contact_model_ = other.contact_model_;
    contact_solver_enum_ = other.contact_solver_enum_;
    sap_near_rigid_threshold_ = other.sap_near_rigid_threshold_;
    contact_surface_representation_ = other.contact_surface_representation_;
    // geometry_query_port_ is set during DeclareSceneGraphPorts() below.
    // geometry_pose_port_ is set during DeclareSceneGraphPorts() below.
    // scene_graph_ is set to nullptr in FinalizePlantOnly() below.

    // The following data member are set in DeclareStateCacheAndPorts()
    // in FinalizePlantOnly():
    //   -instance_actuation_ports_
    //   -actuation_port_
    //   -applied_generalized_force_input_port_
    //   -applied_spatial_force_input_port_
    //   -body_poses_port_
    //   -body_spatial_velocities_port_
    //   -body_spatial_acclerations_port_
    //   -state_output_port_
    //   -instance_state_output_ports_
    //   -generalized_acceleration_output_port_
    //   -instance_generalized_acceleration_output_ports_
    //   -contact_results_port_
    //   -reaction_forces_port_
    //   -instance_generalized_contact_forces_output_ports_

    // Partially copy multibody_graph_. The looped calls to RegisterJointInGraph
    // below copy the second half.
    // TODO(xuchenhan-tri) MultibodyGraph should offer a public function (or
    // constructor) for scalar conversion, so that MbP can just delegate the
    // copying to MbG, instead of leaking knowledge of what kind of data MbG
    // holds into MbP's converting constructor here.
    for (BodyIndex index(0); index < num_bodies(); ++index) {
      const Body<T>& body = get_body(index);
      multibody_graph_.AddBody(body.name(), body.model_instance());
    }

    time_step_ = other.time_step_;
    // discrete_update_manager_ is copied below after FinalizePlantOnly().

    // Copy over physical_models_.
    // Note: The physical models must be cloned before `FinalizePlantOnly()` is
    // called because `FinalizePlantOnly()` has to allocate system resources
    // requested by physical models.
    for (auto& model : other.physical_models_) {
      auto cloned_model = model->template CloneToScalar<T>();
      // TODO(xuchenhan-tri): Rework physical model and discrete update manager
      //  to eliminate the requirement on the order that they are called with
      //  respect to Finalize().

      // AddPhysicalModel can't be called here because it's post-finalize. We
      // have to manually disable scalars that the cloned physical model do not
      // support.
      RemoveUnsupportedScalars(*cloned_model);
      physical_models_.emplace_back(std::move(cloned_model));
    }

    coupler_constraints_specs_ = other.coupler_constraints_specs_;
    distance_constraints_specs_ = other.distance_constraints_specs_;
    ball_constraints_specs_ = other.ball_constraints_specs_;

    // cache_indexes_ is set in DeclareCacheEntries() in
    // DeclareStateCacheAndPorts() in FinalizePlantOnly().

    adjacent_bodies_collision_filters_ =
        other.adjacent_bodies_collision_filters_;
  }

  DeclareSceneGraphPorts();

  for (JointIndex index(0); index < num_joints(); ++index) {
    RegisterJointInGraph(get_joint(index));
  }

  // MultibodyTree::CloneToScalar() already called MultibodyTree::Finalize()
  // on the new MultibodyTree on U. Therefore we only Finalize the plant's
  // internals (and not the MultibodyTree).
  FinalizePlantOnly();

  // Note: The discrete update manager needs to be copied *after* the plant is
  // finalized.
  if (other.discrete_update_manager_ != nullptr) {
    SetDiscreteUpdateManager(
        other.discrete_update_manager_->template CloneToScalar<T>());
  }
}

template <typename T>
ConstraintIndex MultibodyPlant<T>::AddCouplerConstraint(const Joint<T>& joint0,
                                                        const Joint<T>& joint1,
                                                        double gear_ratio,
                                                        double offset) {
  // N.B. The manager is setup at Finalize() and therefore we must require
  // constraints to be added pre-finalize.
  DRAKE_MBP_THROW_IF_FINALIZED();

  if (!is_discrete()) {
    throw std::runtime_error(
        "Currently coupler constraints are only supported for discrete "
        "MultibodyPlant models.");
  }

  // TAMSI does not support coupler constraints. For all other solvers, we let
  // the discrete update manager to throw an exception at finalize time.
  if (contact_solver_enum_ == DiscreteContactSolver::kTamsi) {
    throw std::runtime_error(
        "Currently this MultibodyPlant is set to use the TAMSI solver. TAMSI "
        "does not support coupler constraints. Use "
        "set_discrete_contact_solver() to set a different solver type.");
  }

  if (joint0.num_velocities() != 1 || joint1.num_velocities() != 1) {
    const std::string message = fmt::format(
        "Coupler constraints can only be defined on single-DOF joints. "
        "However joint '{}' has {} DOFs and joint '{}' has {} "
        "DOFs.",
        joint0.name(), joint0.num_velocities(), joint1.name(),
        joint1.num_velocities());
    throw std::runtime_error(message);
  }

  const ConstraintIndex constraint_index(num_constraints());

  coupler_constraints_specs_.push_back(internal::CouplerConstraintSpecs{
      joint0.index(), joint1.index(), gear_ratio, offset});

  return constraint_index;
}

template <typename T>
ConstraintIndex MultibodyPlant<T>::AddDistanceConstraint(
    const Body<T>& body_A, const Vector3<double>& p_AP, const Body<T>& body_B,
    const Vector3<double>& p_BQ, double distance, double stiffness,
    double damping) {
  // N.B. The manager is setup at Finalize() and therefore we must require
  // constraints to be added pre-finalize.
  DRAKE_MBP_THROW_IF_FINALIZED();

  if (!is_discrete()) {
    throw std::runtime_error(
        "Currently distance constraints are only supported for discrete "
        "MultibodyPlant models.");
  }

  // TAMSI does not support distance constraints. For all other solvers, we let
  // the discrete update manager throw an exception at finalize time.
  if (contact_solver_enum_ == DiscreteContactSolver::kTamsi) {
    throw std::runtime_error(
        "Currently this MultibodyPlant is set to use the TAMSI solver. TAMSI "
        "does not support distance constraints. Use "
        "set_discrete_contact_solver(DiscreteContactSolver::kSap) to use the "
        "SAP solver instead. For other solvers, refer to "
        "DiscreteContactSolver.");
  }

  DRAKE_THROW_UNLESS(body_A.index() != body_B.index());

  internal::DistanceConstraintSpecs spec{body_A.index(), p_AP, body_B.index(),
                                         p_BQ, distance, stiffness, damping};
  if (!spec.IsValid()) {
    const std::string msg = fmt::format(
        "Invalid set of parameters for constraint between bodies '{}' and "
        "'{}'. distance = {}, stiffness = {}, damping = {}.",
        body_A.name(), body_B.name(), distance, stiffness, damping);
    throw std::runtime_error(msg);
  }

  const ConstraintIndex constraint_index(num_constraints());

  distance_constraints_specs_.push_back(spec);

  return constraint_index;
}

template <typename T>
ConstraintIndex MultibodyPlant<T>::AddBallConstraint(
    const Body<T>& body_A, const Vector3<double>& p_AP, const Body<T>& body_B,
    const Vector3<double>& p_BQ) {
  // N.B. The manager is set up at Finalize() and therefore we must require
  // constraints to be added pre-finalize.
  DRAKE_MBP_THROW_IF_FINALIZED();

  if (!is_discrete()) {
    throw std::runtime_error(
        "Currently ball constraints are only supported for discrete "
        "MultibodyPlant models.");
  }

  // TAMSI does not support ball constraints. For all other solvers, we let
  // the discrete update manager throw an exception at finalize time.
  if (contact_solver_enum_ == DiscreteContactSolver::kTamsi) {
    throw std::runtime_error(
        "Currently this MultibodyPlant is set to use the TAMSI solver. TAMSI "
        "does not support ball constraints. Use "
        "set_discrete_contact_solver(DiscreteContactSolver::kSap) to use the "
        "SAP solver instead. For other solvers, refer to "
        "DiscreteContactSolver.");
  }

  internal::BallConstraintSpecs spec{body_A.index(), p_AP, body_B.index(),
                                     p_BQ};
  if (!spec.IsValid()) {
    const std::string msg = fmt::format(
        "Invalid set of parameters for constraint between bodies '{}' and "
        "'{}'. For a ball constraint, points P and Q must be on two distinct "
        "bodies, i.e. body_A != body_B must be satisfied.",
        body_A.name(), body_B.name());
    throw std::logic_error(msg);
  }

  const ConstraintIndex constraint_index(num_constraints());

  ball_constraints_specs_.push_back(spec);

  return constraint_index;
}

template <typename T>
std::string MultibodyPlant<T>::GetTopologyGraphvizString() const {
  std::string graphviz = "digraph MultibodyPlant {\n";
  graphviz += "label=\"" + this->get_name() + "\";\n";
  graphviz += "rankdir=BT;\n";
  graphviz += "labelloc=t;\n";
  // Create a subgraph for each model instance, with the bodies as nodes.
  // Note that the subgraph name must have the "cluster" prefix in order to
  // have the box drawn.
  for (ModelInstanceIndex model_instance_index(0);
       model_instance_index < num_model_instances(); ++model_instance_index) {
    graphviz += fmt::format("subgraph cluster{} {{\n", model_instance_index);
    graphviz += fmt::format(" label=\"{}\";\n",
                            GetModelInstanceName(model_instance_index));
    for (const BodyIndex& body_index : GetBodyIndices(model_instance_index)) {
      const Body<T>& body = get_body(body_index);
      graphviz +=
          fmt::format(" body{} [label=\"{}\"];\n", body.index(), body.name());
    }
    graphviz += "}\n";
  }
  // Add the graph edges (via the joints).
  for (JointIndex joint_index(0); joint_index < num_joints(); ++joint_index) {
    const Joint<T>& joint = get_joint(joint_index);
    graphviz += fmt::format(
        "body{} -> body{} [label=\"{} [{}]\"];\n", joint.child_body().index(),
        joint.parent_body().index(), joint.name(), joint.type_name());
  }
  // TODO(russt): Consider adding actuators, frames, forces, etc.
  graphviz += "}\n";
  return graphviz;
}

template <typename T>
void MultibodyPlant<T>::set_contact_model(ContactModel model) {
  DRAKE_MBP_THROW_IF_FINALIZED();
  contact_model_ = model;
}

template <typename T>
void MultibodyPlant<T>::set_discrete_contact_solver(
    DiscreteContactSolver contact_solver) {
  DRAKE_MBP_THROW_IF_FINALIZED();
  contact_solver_enum_ = contact_solver;
}

template <typename T>
DiscreteContactSolver MultibodyPlant<T>::get_discrete_contact_solver()
    const {
  return contact_solver_enum_;
}

template <typename T>
void MultibodyPlant<T>::set_sap_near_rigid_threshold(
    double near_rigid_threshold) {
  DRAKE_MBP_THROW_IF_FINALIZED();
  DRAKE_THROW_UNLESS(near_rigid_threshold >= 0.0);
  sap_near_rigid_threshold_ = near_rigid_threshold;
}

template <typename T>
double MultibodyPlant<T>::get_sap_near_rigid_threshold() const {
  return sap_near_rigid_threshold_;
}

template <typename T>
ContactModel MultibodyPlant<T>::get_contact_model() const {
  return contact_model_;
}

template <typename T>
void MultibodyPlant<T>::SetFreeBodyRandomRotationDistributionToUniform(
    const Body<T>& body) {
  RandomGenerator generator;
  auto q_FM =
      math::UniformlyRandomQuaternion<symbolic::Expression>(&generator);
  SetFreeBodyRandomRotationDistribution(body, q_FM);
}

template <typename T>
const WeldJoint<T>& MultibodyPlant<T>::WeldFrames(
    const Frame<T>& frame_on_parent_F, const Frame<T>& frame_on_child_M,
    const math::RigidTransform<double>& X_FM) {
  const std::string joint_name =
      frame_on_parent_F.name() + "_welds_to_" + frame_on_child_M.name();
  return AddJoint(std::make_unique<WeldJoint<T>>(joint_name, frame_on_parent_F,
                                                 frame_on_child_M, X_FM));
}

template <typename T>
const JointActuator<T>& MultibodyPlant<T>::AddJointActuator(
    const std::string& name, const Joint<T>& joint,
    double effort_limit) {
  if (joint.num_velocities() != 1) {
    throw std::logic_error(fmt::format(
        "Calling AddJointActuator with joint {} failed -- this joint has "
        "{} degrees of freedom, and MultibodyPlant currently only "
        "supports actuators for single degree-of-freedom joints. "
        "See https://stackoverflow.com/q/71477852/9510020 for "
        "the common workarounds.",
        joint.name(), joint.num_velocities()));
  }
  return this->mutable_tree().AddJointActuator(name, joint, effort_limit);
}

template <typename T>
geometry::SourceId MultibodyPlant<T>::RegisterAsSourceForSceneGraph(
    SceneGraph<T>* scene_graph) {
  DRAKE_THROW_UNLESS(scene_graph != nullptr);
  DRAKE_THROW_UNLESS(!geometry_source_is_registered());
  // Save the GS pointer so that on later geometry registrations can use this
  // instance. This will be nullified at Finalize().
  scene_graph_ = scene_graph;
  source_id_ = scene_graph_->RegisterSource(this->get_name());
  const geometry::FrameId world_frame_id = scene_graph_->world_frame_id();
  body_index_to_frame_id_[world_index()] = world_frame_id;
  frame_id_to_body_index_[world_frame_id] = world_index();
  // In case any bodies were added before registering scene graph, make sure the
  // bodies get their corresponding geometry frame ids.
  RegisterGeometryFramesForAllBodies();
  return source_id_.value();
}

template <typename T>
geometry::GeometryId MultibodyPlant<T>::RegisterVisualGeometry(
    const Body<T>& body, const math::RigidTransform<double>& X_BG,
    const geometry::Shape& shape, const std::string& name) {
  return RegisterVisualGeometry(
      body, X_BG, shape, name, geometry::IllustrationProperties());
}

template <typename T>
geometry::GeometryId MultibodyPlant<T>::RegisterVisualGeometry(
    const Body<T>& body, const math::RigidTransform<double>& X_BG,
    const geometry::Shape& shape, const std::string& name,
    const Vector4<double>& diffuse_color) {
  return RegisterVisualGeometry(
      body, X_BG, shape, name,
      geometry::MakePhongIllustrationProperties(diffuse_color));
}

template <typename T>
geometry::GeometryId MultibodyPlant<T>::RegisterVisualGeometry(
    const Body<T>& body, const math::RigidTransform<double>& X_BG,
    const geometry::Shape& shape, const std::string& name,
    const geometry::IllustrationProperties& properties) {
  // TODO(SeanCurtis-TRI): Consider simply adding an interface that takes a
  // unique pointer to an already instantiated GeometryInstance. This will
  // require shuffling around a fair amount of code and should ultimately be
  // supplanted by providing a cleaner interface between parsing MBP and SG
  // elements.
  DRAKE_MBP_THROW_IF_FINALIZED();
  DRAKE_THROW_UNLESS(geometry_source_is_registered());

  // TODO(amcastro-tri): Consider doing this after finalize so that we can
  // register geometry that has a fixed path to world to the world body (i.e.,
  // as anchored geometry).
  GeometryId id =
      RegisterGeometry(body, X_BG, shape,
                       GetScopedName(*this, body.model_instance(), name));
  scene_graph_->AssignRole(*source_id_, id, properties);

  // TODO(SeanCurtis-TRI): Eliminate the automatic assignment of perception
  //  and illustration in favor of a protocol that allows definition.
  geometry::PerceptionProperties perception_props;
  perception_props.AddProperty("label", "id", RenderLabel(body.index()));
  perception_props.AddProperty(
      "phong", "diffuse",
      properties.GetPropertyOrDefault(
          "phong", "diffuse", Vector4<double>(0.9, 0.9, 0.9, 1.0)));
  if (properties.HasProperty("phong", "diffuse_map")) {
    perception_props.AddProperty(
        "phong", "diffuse_map",
        properties.GetProperty<std::string>("phong", "diffuse_map"));
  }
  if (properties.HasProperty("renderer", "accepting")) {
    perception_props.AddProperty(
      "renderer", "accepting",
      properties.GetProperty<std::set<std::string>>("renderer", "accepting"));
  }
  scene_graph_->AssignRole(*source_id_, id, perception_props);

  DRAKE_ASSERT(ssize(visual_geometries_) == num_bodies());
  visual_geometries_[body.index()].push_back(id);
  ++num_visual_geometries_;
  return id;
}

template <typename T>
const std::vector<geometry::GeometryId>&
MultibodyPlant<T>::GetVisualGeometriesForBody(const Body<T>& body) const {
  return visual_geometries_[body.index()];
}

template <typename T>
geometry::GeometryId MultibodyPlant<T>::RegisterCollisionGeometry(
    const Body<T>& body, const math::RigidTransform<double>& X_BG,
    const geometry::Shape& shape, const std::string& name,
    geometry::ProximityProperties properties) {
  DRAKE_MBP_THROW_IF_FINALIZED();
  DRAKE_THROW_UNLESS(geometry_source_is_registered());
  DRAKE_THROW_UNLESS(properties.HasProperty(geometry::internal::kMaterialGroup,
                                            geometry::internal::kFriction));

  // TODO(amcastro-tri): Consider doing this after finalize so that we can
  // register geometry that has a fixed path to world to the world body (i.e.,
  // as anchored geometry).
  GeometryId id = RegisterGeometry(
      body, X_BG, shape, GetScopedName(*this, body.model_instance(), name));

  scene_graph_->AssignRole(*source_id_, id, std::move(properties));
  DRAKE_ASSERT(ssize(collision_geometries_) == num_bodies());
  collision_geometries_[body.index()].push_back(id);
  ++num_collision_geometries_;
  return id;
}

template <typename T>
geometry::GeometryId MultibodyPlant<T>::RegisterCollisionGeometry(
    const Body<T>& body, const math::RigidTransform<double>& X_BG,
    const geometry::Shape& shape, const std::string& name,
    const CoulombFriction<double>& coulomb_friction) {
  geometry::ProximityProperties props;
  props.AddProperty(geometry::internal::kMaterialGroup,
                    geometry::internal::kFriction, coulomb_friction);
  return RegisterCollisionGeometry(body, X_BG, shape, name, std::move(props));
}

template <typename T>
const std::vector<geometry::GeometryId>&
MultibodyPlant<T>::GetCollisionGeometriesForBody(const Body<T>& body) const {
  DRAKE_ASSERT(body.index() < num_bodies());
  return collision_geometries_[body.index()];
}

template <typename T>
geometry::GeometrySet MultibodyPlant<T>::CollectRegisteredGeometries(
    const std::vector<const Body<T>*>& bodies) const {
  DRAKE_THROW_UNLESS(geometry_source_is_registered());

  geometry::GeometrySet geometry_set;
  for (const Body<T>* body : bodies) {
    std::optional<FrameId> frame_id = GetBodyFrameIdIfExists(body->index());
    if (frame_id) {
      geometry_set.Add(frame_id.value());
    }
  }
  return geometry_set;
}

template <typename T>
std::vector<const Body<T>*> MultibodyPlant<T>::GetBodiesWeldedTo(
    const Body<T>& body) const {
  const std::set<BodyIndex> island =
      multibody_graph_.FindBodiesWeldedTo(body.index());
  // Map body indices to pointers.
  std::vector<const Body<T>*> sub_graph_bodies;
  for (BodyIndex body_index : island) {
    sub_graph_bodies.push_back(&get_body(body_index));
  }
  return sub_graph_bodies;
}

template <typename T>
std::vector<BodyIndex> MultibodyPlant<T>::GetBodiesKinematicallyAffectedBy(
    const std::vector<JointIndex>& joint_indexes) const {
  DRAKE_MBP_THROW_IF_NOT_FINALIZED();
  for (const JointIndex& joint : joint_indexes) {
    if (!joint.is_valid() || joint >= num_joints()) {
      throw std::logic_error(fmt::format(
          "{}: No joint with index {} has been registered.", __func__, joint));
    }
    if (get_joint(joint).num_velocities() == 0) {
      throw std::logic_error(fmt::format(
          "{}: joint with index {} is welded.", __func__, joint));
    }
  }
  return internal_tree().GetBodiesKinematicallyAffectedBy(joint_indexes);
}

template <typename T>
std::unordered_set<BodyIndex> MultibodyPlant<T>::GetFloatingBaseBodies() const {
  DRAKE_MBP_THROW_IF_NOT_FINALIZED();
  std::unordered_set<BodyIndex> floating_bodies;
  for (BodyIndex body_index(0); body_index < num_bodies(); ++body_index) {
    const Body<T>& body = get_body(body_index);
    if (body.is_floating()) floating_bodies.insert(body.index());
  }
  return floating_bodies;
}

template <typename T>
geometry::GeometryId MultibodyPlant<T>::RegisterGeometry(
    const Body<T>& body, const math::RigidTransform<double>& X_BG,
    const geometry::Shape& shape,
    const std::string& name) {
  DRAKE_ASSERT(!is_finalized());
  DRAKE_ASSERT(geometry_source_is_registered());
  DRAKE_ASSERT(body_has_registered_frame(body));

  // Register geometry in the body frame.
  std::unique_ptr<geometry::GeometryInstance> geometry_instance =
      std::make_unique<GeometryInstance>(X_BG, shape.Clone(), name);
  GeometryId geometry_id = scene_graph_->RegisterGeometry(
      source_id_.value(), body_index_to_frame_id_[body.index()],
      std::move(geometry_instance));
  geometry_id_to_body_index_[geometry_id] = body.index();
  return geometry_id;
}

template <typename T>
void MultibodyPlant<T>::RegisterGeometryFramesForAllBodies() {
  DRAKE_ASSERT(geometry_source_is_registered());
  // Loop through the bodies to make sure that all bodies get a geometry frame.
  // If not, create and attach one.
  for (BodyIndex body_index(0); body_index < num_bodies(); ++body_index) {
    const auto& body = get_body(body_index);
    RegisterRigidBodyWithSceneGraph(body);
  }
}

template <typename T>
void MultibodyPlant<T>::RegisterRigidBodyWithSceneGraph(
    const Body<T>& body) {
  if (geometry_source_is_registered()) {
    // If not already done, register a frame for this body.
    if (!body_has_registered_frame(body)) {
      FrameId frame_id = scene_graph_->RegisterFrame(
          source_id_.value(),
          GeometryFrame(
              GetScopedName(*this, body.model_instance(), body.name()),
              /* TODO(@SeanCurtis-TRI): Add test coverage for this
               * model-instance support as requested in #9390. */
              body.model_instance()));
      body_index_to_frame_id_[body.index()] = frame_id;
      frame_id_to_body_index_[frame_id] = body.index();
    }
  }
}

template<typename T>
void MultibodyPlant<T>::SetFreeBodyPoseInWorldFrame(
    systems::Context<T>* context,
    const Body<T>& body, const math::RigidTransform<T>& X_WB) const {
  DRAKE_MBP_THROW_IF_NOT_FINALIZED();
  this->ValidateContext(context);
  internal_tree().SetFreeBodyPoseOrThrow(body, X_WB, context);
}

template<typename T>
void MultibodyPlant<T>::SetFreeBodyPoseInAnchoredFrame(
    systems::Context<T>* context,
    const Frame<T>& frame_F, const Body<T>& body,
    const math::RigidTransform<T>& X_FB) const {
  DRAKE_MBP_THROW_IF_NOT_FINALIZED();
  this->ValidateContext(context);

  if (!internal_tree().get_topology().IsBodyAnchored(frame_F.body().index())) {
    throw std::logic_error(
        "Frame '" + frame_F.name() + "' must be anchored to the world frame.");
  }

  // Pose of frame F in its parent body frame P.
  const RigidTransform<T> X_PF = frame_F.GetFixedPoseInBodyFrame();
  // Pose of frame F's parent body P in the world.
  const RigidTransform<T>& X_WP = EvalBodyPoseInWorld(*context, frame_F.body());
  // Pose of "body" B in the world frame.
  const RigidTransform<T> X_WB = X_WP * X_PF * X_FB;
  SetFreeBodyPoseInWorldFrame(context, body, X_WB);
}

template <typename T>
void MultibodyPlant<T>::CalcSpatialAccelerationsFromVdot(
    const systems::Context<T>& context, const VectorX<T>& known_vdot,
    std::vector<SpatialAcceleration<T>>* A_WB_array) const {
  this->ValidateContext(context);
  DRAKE_THROW_UNLESS(A_WB_array != nullptr);
  DRAKE_THROW_UNLESS(ssize(*A_WB_array) == num_bodies());
  internal_tree().CalcSpatialAccelerationsFromVdot(
      context, internal_tree().EvalPositionKinematics(context),
      internal_tree().EvalVelocityKinematics(context), known_vdot, A_WB_array);
  // Permute BodyNodeIndex -> BodyIndex.
  // TODO(eric.cousineau): Remove dynamic allocations. Making this in-place
  // still required dynamic allocation for recording permutation indices.
  // Can change implementation once MultibodyTree becomes fully internal.
  std::vector<SpatialAcceleration<T>> A_WB_array_node = *A_WB_array;
  const internal::MultibodyTreeTopology& topology =
      internal_tree().get_topology();
  for (internal::BodyNodeIndex node_index(1);
       node_index < topology.get_num_body_nodes(); ++node_index) {
    const BodyIndex body_index = topology.get_body_node(node_index).body;
    (*A_WB_array)[body_index] = A_WB_array_node[node_index];
  }
}

template<typename T>
void MultibodyPlant<T>::CalcForceElementsContribution(
      const systems::Context<T>& context,
      MultibodyForces<T>* forces) const {
  this->ValidateContext(context);
  DRAKE_THROW_UNLESS(forces != nullptr);
  DRAKE_THROW_UNLESS(forces->CheckHasRightSizeForModel(internal_tree()));
  internal_tree().CalcForceElementsContribution(
      context, EvalPositionKinematics(context),
      EvalVelocityKinematics(context),
      forces);
}

template<typename T>
void MultibodyPlant<T>::Finalize() {
  // After finalizing the base class, tree is read-only.
  internal::MultibodyTreeSystem<T>::Finalize();

  // Add free joints created by tree's finalize to the multibody graph.
  // Until the call to Finalize(), all joints are added through calls to
  // MultibodyPlant APIs and therefore registered in the graph. This accounts
  // for the QuaternionFloatingJoint added for each free body that was not
  // explicitly given a parent joint. It is important that this loop happens
  // AFTER finalizing the internal tree.
  for (JointIndex i{multibody_graph_.num_joints()}; i < num_joints(); ++i) {
    RegisterJointInGraph(get_joint(i));
  }

  if (geometry_source_is_registered()) {
    ApplyDefaultCollisionFilters();
    ExcludeCollisionsWithVisualGeometry();
  }
  FinalizePlantOnly();

  // Make the manager of discrete updates.
  if (is_discrete()) {
    std::unique_ptr<internal::DiscreteUpdateManager<T>> manager =
        internal::MakeDiscreteUpdateManager<T>(contact_solver_enum_);
    if (manager) {
      SetDiscreteUpdateManager(std::move(manager));
    }
  }
}

template<typename T>
void MultibodyPlant<T>::SetUpJointLimitsParameters() {
  for (JointIndex joint_index(0); joint_index < num_joints();
       ++joint_index) {
    // Currently MultibodyPlant applies these "compliant" joint limit forces
    // using an explicit Euler strategy. Stability analysis of the explicit
    // Euler applied to the harmonic oscillator (the model used for these
    // compliant forces) shows the scheme to be stable for kAlpha > 2π. We take
    // a significantly larger kAlpha so that we are well within the stability
    // region of the scheme.
    // TODO(amcastro-tri): Decrease the value of kAlpha to be closer to one when
    // the time stepping scheme is updated to be implicit in the joint limits.
    const double kAlpha = 20 * M_PI;

    const Joint<T>& joint = get_joint(joint_index);
    auto revolute_joint = dynamic_cast<const RevoluteJoint<T>*>(&joint);
    auto prismatic_joint = dynamic_cast<const PrismaticJoint<T>*>(&joint);
    // Currently MBP only supports limits for prismatic and revolute joints.
    if (!(revolute_joint || prismatic_joint)) continue;

    const double penalty_time_scale = kAlpha * time_step();

    if (revolute_joint) {
      const double lower_limit = revolute_joint->position_lower_limits()(0);
      const double upper_limit = revolute_joint->position_upper_limits()(0);
      // We only compute parameters if joints do have upper/lower bounds.
      if (!std::isinf(lower_limit) || !std::isinf(upper_limit)) {
        joint_limits_parameters_.joints_with_limits.push_back(
            revolute_joint->index());

        // Store joint limits.
        joint_limits_parameters_.lower_limit.push_back(lower_limit);
        joint_limits_parameters_.upper_limit.push_back(upper_limit);
        // Estimate penalty parameters.
        auto penalty_parameters =
            internal::JointLimitsPenaltyParametersEstimator<T>::
            CalcRevoluteJointPenaltyParameters(
                *revolute_joint, penalty_time_scale);
        joint_limits_parameters_.stiffness.push_back(penalty_parameters.first);
        joint_limits_parameters_.damping.push_back(penalty_parameters.second);
      }
    }

    if (prismatic_joint) {
      const double lower_limit = prismatic_joint->position_lower_limits()(0);
      const double upper_limit = prismatic_joint->position_upper_limits()(0);
      // We only compute parameters if joints do have upper/lower bounds.
      if (!std::isinf(lower_limit) || !std::isinf(upper_limit)) {
        joint_limits_parameters_.joints_with_limits.push_back(
            prismatic_joint->index());

        // Store joint limits.
        joint_limits_parameters_.lower_limit.push_back(lower_limit);
        joint_limits_parameters_.upper_limit.push_back(upper_limit);

        // Estimate penalty parameters.
        auto penalty_parameters =
            internal::JointLimitsPenaltyParametersEstimator<T>::
            CalcPrismaticJointPenaltyParameters(
                *prismatic_joint, penalty_time_scale);
        joint_limits_parameters_.stiffness.push_back(penalty_parameters.first);
        joint_limits_parameters_.damping.push_back(penalty_parameters.second);
      }
    }
  }

  // Since currently MBP only handles joint limits for discrete models, we
  // verify that there are no joint limits when the model is continuous.
  // If there are limits defined, we prepare a warning message that will be
  // logged iff the user attempts to do anything that would have needed them.
  if (!is_discrete() && !joint_limits_parameters_.joints_with_limits.empty()) {
    std::string joint_names_with_limits;
    for (auto joint_index : joint_limits_parameters_.joints_with_limits) {
      joint_names_with_limits += fmt::format(
          ", '{}'", get_joint(joint_index).name());
    }
    joint_names_with_limits = joint_names_with_limits.substr(2);  // Nix ", ".
    joint_limits_parameters_.pending_warning_message =
        "Currently MultibodyPlant does not handle joint limits for continuous "
        "models. However some joints do specify limits. Consider setting a "
        "non-zero time step in the MultibodyPlant constructor; this will put "
        "the plant in discrete-time mode, which does support joint limits. "
        "Joints that specify limits are: " + joint_names_with_limits;
  }
}

template<typename T>
void MultibodyPlant<T>::FinalizePlantOnly() {
  DeclareStateCacheAndPorts();
  if (num_collision_geometries() > 0 &&
      penalty_method_contact_parameters_.time_scale < 0)
    EstimatePointContactParameters(penetration_allowance_);
  if (num_collision_geometries() > 0 &&
      friction_model_.stiction_tolerance() < 0)
    set_stiction_tolerance();
  SetUpJointLimitsParameters();
  scene_graph_ = nullptr;  // must not be used after Finalize().
}

template <typename T>
MatrixX<T> MultibodyPlant<T>::MakeActuationMatrix() const {
  MatrixX<T> B = MatrixX<T>::Zero(num_velocities(), num_actuated_dofs());
  for (JointActuatorIndex actuator_index(0);
       actuator_index < num_actuators(); ++actuator_index) {
    const JointActuator<T>& actuator = get_joint_actuator(actuator_index);
    // This method assumes actuators on single dof joints. Assert this
    // condition.
    DRAKE_DEMAND(actuator.joint().num_velocities() == 1);
    B(actuator.joint().velocity_start(), int{actuator.index()}) = 1;
  }
  return B;
}

namespace {

void ThrowForDisconnectedGeometryPort(std::string_view explanation) {
  throw std::logic_error(
      std::string(explanation) +
      "\n\nThe provided context doesn't show a connection for the plant's "
      "query input port (see MultibodyPlant::get_geometry_query_input_port())"
      ". See https://drake.mit.edu/troubleshooting.html"
      "#mbp-unconnected-query-object-port for help.");
}

}  // namespace

template <typename T>
const geometry::QueryObject<T>& MultibodyPlant<T>::EvalGeometryQueryInput(
    const systems::Context<T>& context, std::string_view explanation) const {
  this->ValidateContext(context);
  if (!get_geometry_query_input_port().HasValue(context)) {
    ThrowForDisconnectedGeometryPort(explanation);
  }
  return get_geometry_query_input_port()
      .template Eval<geometry::QueryObject<T>>(context);
}

template <typename T>
void MultibodyPlant<T>::ValidateGeometryInput(
    const systems::Context<T>& context, std::string_view explanation) const {
  if (!IsValidGeometryInput(context)) {
    ThrowForDisconnectedGeometryPort(explanation);
  }
}

template <typename T>
void MultibodyPlant<T>::ValidateGeometryInput(
    const systems::Context<T>& context,
    const systems::OutputPort<T>& output_port) const {
  if (!IsValidGeometryInput(context)) {
    ThrowForDisconnectedGeometryPort(fmt::format(
        "You've tried evaluating MultibodyPlant's '{}' output port.",
        output_port.get_name()));
  }
}

template <typename T>
bool MultibodyPlant<T>::IsValidGeometryInput(
    const systems::Context<T>& context) const {
  return num_collision_geometries() == 0 ||
         get_geometry_query_input_port().HasValue(context);
}

template <typename T>
std::pair<T, T> MultibodyPlant<T>::GetPointContactParameters(
    geometry::GeometryId id,
    const geometry::SceneGraphInspector<T>& inspector) const {
  const geometry::ProximityProperties* prop =
      inspector.GetProximityProperties(id);
  DRAKE_DEMAND(prop != nullptr);
  return std::pair(prop->template GetPropertyOrDefault<T>(
                       geometry::internal::kMaterialGroup,
                       geometry::internal::kPointStiffness,
                       penalty_method_contact_parameters_.geometry_stiffness),
                   prop->template GetPropertyOrDefault<T>(
                       geometry::internal::kMaterialGroup,
                       geometry::internal::kHcDissipation,
                       penalty_method_contact_parameters_.dissipation));
}

template <typename T>
const CoulombFriction<double>& MultibodyPlant<T>::GetCoulombFriction(
    geometry::GeometryId id,
    const geometry::SceneGraphInspector<T>& inspector) const {
  const geometry::ProximityProperties* prop =
      inspector.GetProximityProperties(id);
  DRAKE_DEMAND(prop != nullptr);
  DRAKE_THROW_UNLESS(prop->HasProperty(geometry::internal::kMaterialGroup,
                                       geometry::internal::kFriction));
  return prop->GetProperty<CoulombFriction<double>>(
      geometry::internal::kMaterialGroup, geometry::internal::kFriction);
}

template <typename T>
void MultibodyPlant<T>::ApplyDefaultCollisionFilters() {
  DRAKE_DEMAND(geometry_source_is_registered());
  if (adjacent_bodies_collision_filters_) {
    // Disallow collisions between adjacent bodies. Adjacency is implied by the
    // existence of a joint between bodies, except in the case of 6-dof joints
    // or joints in which the parent body is `world`.
    for (JointIndex j{0}; j < num_joints(); ++j) {
      const Joint<T>& joint = get_joint(j);
      const Body<T>& child = joint.child_body();
      const Body<T>& parent = joint.parent_body();
      if (parent.index() == world_index()) continue;
      if (joint.type_name() == QuaternionFloatingJoint<T>::kTypeName) continue;
      std::optional<FrameId> child_id = GetBodyFrameIdIfExists(child.index());
      std::optional<FrameId> parent_id = GetBodyFrameIdIfExists(parent.index());

      if (child_id && parent_id) {
        scene_graph_->collision_filter_manager().Apply(
            CollisionFilterDeclaration().ExcludeBetween(
                geometry::GeometrySet(*child_id),
                geometry::GeometrySet(*parent_id)));
      }
    }
  }
  // We explicitly exclude collisions within welded subgraphs.
  std::vector<std::set<BodyIndex>> subgraphs =
      multibody_graph_.FindSubgraphsOfWeldedBodies();
  for (const auto& subgraph : subgraphs) {
    // Only operate on non-trivial weld subgraphs.
    if (subgraph.size() <= 1) { continue; }
    // Map body indices to pointers.
    std::vector<const Body<T>*> subgraph_bodies;
    for (BodyIndex body_index : subgraph) {
      subgraph_bodies.push_back(&get_body(body_index));
    }
    auto geometries = CollectRegisteredGeometries(subgraph_bodies);
    scene_graph_->collision_filter_manager().Apply(
        CollisionFilterDeclaration().ExcludeWithin(geometries));
  }
}

template <typename T>
void MultibodyPlant<T>::ExcludeCollisionsWithVisualGeometry() {
  DRAKE_DEMAND(geometry_source_is_registered());
  geometry::GeometrySet visual;
  for (const auto& body_geometries : visual_geometries_) {
    visual.Add(body_geometries);
  }
  geometry::GeometrySet collision;
  for (const auto& body_geometries : collision_geometries_) {
    collision.Add(body_geometries);
  }
  // clang-format off
  scene_graph_->collision_filter_manager().Apply(
      CollisionFilterDeclaration()
          .ExcludeWithin(visual)
          .ExcludeBetween(visual, collision));
  // clang-format on
}

template <typename T>
void MultibodyPlant<T>::ExcludeCollisionGeometriesWithCollisionFilterGroupPair(
    const std::pair<std::string, geometry::GeometrySet>&
        collision_filter_group_a,
    const std::pair<std::string, geometry::GeometrySet>&
        collision_filter_group_b) {
  DRAKE_DEMAND(!is_finalized());
  DRAKE_DEMAND(geometry_source_is_registered());

  if (collision_filter_group_a.first == collision_filter_group_b.first) {
    scene_graph_->collision_filter_manager().Apply(
        CollisionFilterDeclaration().ExcludeWithin(
            collision_filter_group_a.second));
  } else {
    scene_graph_->collision_filter_manager().Apply(
        CollisionFilterDeclaration().ExcludeBetween(
            collision_filter_group_a.second, collision_filter_group_b.second));
  }
}

template <typename T>
BodyIndex MultibodyPlant<T>::FindBodyByGeometryId(
    GeometryId geometry_id) const {
  if (!geometry_id.is_valid()) {
    throw std::logic_error(
        "MultibodyPlant received contact results for a null GeometryId");
  }
  const auto iter = geometry_id_to_body_index_.find(geometry_id);
  if (iter != geometry_id_to_body_index_.end()) {
    return iter->second;
  }
  throw std::logic_error(fmt::format(
      "MultibodyPlant received contact results for GeometryId {}, but that"
      " ID is not known to this plant", geometry_id));
}

template <typename T>
void MultibodyPlant<T>::SetDiscreteUpdateManager(
    std::unique_ptr<internal::DiscreteUpdateManager<T>> manager) {
  // N.B. This requirement is really more important on the side of the
  // manager's constructor, since most likely it'll need MBP's topology at
  // least to build the contact problem. However, here we play safe and demand
  // finalization right here.
  DRAKE_MBP_THROW_IF_NOT_FINALIZED();
  DRAKE_DEMAND(is_discrete());
  DRAKE_DEMAND(manager != nullptr);
  manager->SetOwningMultibodyPlant(this);
  discrete_update_manager_ = std::move(manager);
  RemoveUnsupportedScalars(*discrete_update_manager_);
}

template <typename T>
void MultibodyPlant<T>::AddPhysicalModel(
    std::unique_ptr<PhysicalModel<T>> model) {
  // TODO(xuchenhan-tri): Guard against the same type of model being registered
  //  more than once.
  DRAKE_MBP_THROW_IF_FINALIZED();
  DRAKE_DEMAND(model != nullptr);
  auto& added_model = physical_models_.emplace_back(std::move(model));
  RemoveUnsupportedScalars(*added_model);
}

template <typename T>
std::vector<const PhysicalModel<T>*> MultibodyPlant<T>::physical_models()
    const {
  std::vector<const PhysicalModel<T>*> result;
  for (const std::unique_ptr<PhysicalModel<T>>& model : physical_models_) {
    result.emplace_back(model.get());
  }
  return result;
}

template <typename T>
void MultibodyPlant<T>::set_penetration_allowance(
    double penetration_allowance) {
  if (penetration_allowance <= 0) {
    throw std::logic_error(
        "set_penetration_allowance(): penetration_allowance must be strictly "
        "positive.");
  }

  penetration_allowance_ = penetration_allowance;
  // We update the point contact parameters when this method is called
  // post-finalize.
  if (this->is_finalized())
    EstimatePointContactParameters(penetration_allowance);
}

template <typename T>
void MultibodyPlant<T>::SetDefaultPositions(
    const Eigen::Ref<const Eigen::VectorXd>& q) {
  DRAKE_MBP_THROW_IF_NOT_FINALIZED();
  DRAKE_THROW_UNLESS(q.size() == num_positions());
  for (int i = 0; i < num_joints(); ++i) {
    Joint<T>& joint = get_mutable_joint(JointIndex(i));
    joint.set_default_positions(
        q.segment(joint.position_start(), joint.num_positions()));
  }
}

template <typename T>
void MultibodyPlant<T>::SetDefaultPositions(ModelInstanceIndex model_instance,
                    const Eigen::Ref<const Eigen::VectorXd>& q_instance) {
  DRAKE_MBP_THROW_IF_NOT_FINALIZED();
  DRAKE_THROW_UNLESS(q_instance.size() == num_positions(model_instance));
  VectorX<T> q_T(num_positions());
  internal_tree().SetPositionsInArray(model_instance, q_instance.cast<T>(),
                                      &q_T);
  Eigen::VectorXd q = ExtractDoubleOrThrow(q_T);
  for (JointIndex i : GetJointIndices(model_instance)) {
    Joint<T>& joint = get_mutable_joint(i);
    joint.set_default_positions(
        q.segment(joint.position_start(), joint.num_positions()));
  }
}

template <typename T>
std::vector<std::string> MultibodyPlant<T>::GetPositionNames(
    bool add_model_instance_prefix, bool always_add_suffix) const {
  DRAKE_MBP_THROW_IF_NOT_FINALIZED();
  std::vector<std::string> names(num_positions());

  for (int joint_index = 0; joint_index < num_joints(); ++joint_index) {
    const Joint<T>& joint = get_joint(JointIndex(joint_index));
    const std::string prefix =
        add_model_instance_prefix
            ? fmt::format("{}_", GetModelInstanceName(joint.model_instance()))
            : "";
    for (int i = 0; i < joint.num_positions(); ++i) {
      const std::string suffix =
          always_add_suffix || joint.num_positions() > 1
              ? fmt::format("_{}", joint.position_suffix(i))
              : "";
      names[joint.position_start() + i] =
          fmt::format("{}{}{}", prefix, joint.name(), suffix);
    }
  }
  return names;
}

template <typename T>
std::vector<std::string> MultibodyPlant<T>::GetPositionNames(
    ModelInstanceIndex model_instance, bool add_model_instance_prefix,
    bool always_add_suffix) const {
  DRAKE_MBP_THROW_IF_NOT_FINALIZED();
  std::vector<std::string> names(num_positions(model_instance));
  std::vector<JointIndex> joint_indices = GetJointIndices(model_instance);
  // The offset into the position array is the position_start of the first
  // mobilizer in the tree; here we just take the minimum.
  int position_offset = num_positions();
  for (const auto& joint_index : joint_indices) {
    position_offset =
        std::min(position_offset, get_joint(joint_index).position_start());
  }

  for (const auto& joint_index : joint_indices) {
    const Joint<T>& joint = get_joint(joint_index);
    // Sanity check: joint positions are in range.
    DRAKE_DEMAND(joint.position_start() >= position_offset);
    DRAKE_DEMAND(joint.position_start() + joint.num_positions() -
                     position_offset <=
                 ssize(names));

    const std::string prefix =
        add_model_instance_prefix
            ? fmt::format("{}_", GetModelInstanceName(model_instance))
            : "";
    for (int i = 0; i < joint.num_positions(); ++i) {
      const std::string suffix =
          always_add_suffix || joint.num_positions() > 1
              ? fmt::format("_{}", joint.position_suffix(i))
              : "";
      names[joint.position_start() + i - position_offset] =
          fmt::format("{}{}{}", prefix, joint.name(), suffix);
    }
  }
  return names;
}

template <typename T>
std::vector<std::string> MultibodyPlant<T>::GetVelocityNames(
    bool add_model_instance_prefix, bool always_add_suffix) const {
  DRAKE_MBP_THROW_IF_NOT_FINALIZED();
  std::vector<std::string> names(num_velocities());

  for (int joint_index = 0; joint_index < num_joints(); ++joint_index) {
    const Joint<T>& joint = get_joint(JointIndex(joint_index));
    const std::string prefix =
        add_model_instance_prefix
            ? fmt::format("{}_", GetModelInstanceName(joint.model_instance()))
            : "";
    for (int i = 0; i < joint.num_velocities(); ++i) {
      const std::string suffix =
          always_add_suffix || joint.num_velocities() > 1
              ? fmt::format("_{}", joint.velocity_suffix(i))
              : "";
      names[joint.velocity_start() + i] =
          fmt::format("{}{}{}", prefix, joint.name(), suffix);
    }
  }
  return names;
}

template <typename T>
std::vector<std::string> MultibodyPlant<T>::GetVelocityNames(
    ModelInstanceIndex model_instance, bool add_model_instance_prefix,
    bool always_add_suffix) const {
  DRAKE_MBP_THROW_IF_NOT_FINALIZED();
  std::vector<std::string> names(num_velocities(model_instance));
  std::vector<JointIndex> joint_indices = GetJointIndices(model_instance);
  // The offset into the velocity array is the velocity_start of the first
  // mobilizer in the tree; here we just take the minimum.
  int velocity_offset = num_velocities();
  for (const auto& joint_index : joint_indices) {
    velocity_offset =
        std::min(velocity_offset, get_joint(joint_index).velocity_start());
  }

  for (const auto& joint_index : joint_indices) {
    const Joint<T>& joint = get_joint(joint_index);
    // Sanity check: joint velocities are in range.
    DRAKE_DEMAND(joint.velocity_start() >= velocity_offset);
    DRAKE_DEMAND(joint.velocity_start() + joint.num_velocities() -
                     velocity_offset <=
                 ssize(names));

    const std::string prefix =
        add_model_instance_prefix
            ? fmt::format("{}_", GetModelInstanceName(model_instance))
            : "";
    for (int i = 0; i < joint.num_velocities(); ++i) {
      const std::string suffix =
          always_add_suffix || joint.num_velocities() > 1
              ? fmt::format("_{}", joint.velocity_suffix(i))
              : "";
      names[joint.velocity_start() + i - velocity_offset] =
          fmt::format("{}{}{}", prefix, joint.name(), suffix);
    }
  }
  return names;
}

template <typename T>
std::vector<std::string> MultibodyPlant<T>::GetStateNames(
    bool add_model_instance_prefix) const {
  std::vector<std::string> names =
      GetPositionNames(add_model_instance_prefix, true /* always_add_suffix */);
  std::vector<std::string> velocity_names =
      GetVelocityNames(add_model_instance_prefix, true /* always_add_suffix */);
  names.insert(names.end(), std::make_move_iterator(velocity_names.begin()),
               std::make_move_iterator(velocity_names.end()));
  return names;
}

template <typename T>
std::vector<std::string> MultibodyPlant<T>::GetStateNames(
    ModelInstanceIndex model_instance, bool add_model_instance_prefix) const {
  std::vector<std::string> names = GetPositionNames(
      model_instance, add_model_instance_prefix, true /* always_add_suffix */);
  std::vector<std::string> velocity_names = GetVelocityNames(
      model_instance, add_model_instance_prefix, true /* always_add_suffix */);
  names.insert(names.end(), std::make_move_iterator(velocity_names.begin()),
               std::make_move_iterator(velocity_names.end()));
  return names;
}

template <typename T>
std::vector<std::string> MultibodyPlant<T>::GetActuatorNames(
    bool add_model_instance_prefix) const {
  DRAKE_MBP_THROW_IF_NOT_FINALIZED();
  std::vector<std::string> names(num_actuators());

  for (int actuator_index = 0; actuator_index < num_actuators();
       ++actuator_index) {
    const JointActuator<T>& actuator =
        get_joint_actuator(JointActuatorIndex(actuator_index));
    const std::string prefix =
        add_model_instance_prefix
            ? fmt::format("{}_",
                          GetModelInstanceName(actuator.model_instance()))
            : "";
    // TODO(russt): Need to add actuator name suffix to JointActuator and loop
    // over actuator.num_inputs() if we ever actually support actuators with
    // multiple inputs.
    DRAKE_DEMAND(actuator.num_inputs() == 1);
    names[actuator.input_start()] =
        fmt::format("{}{}", prefix, actuator.name());
  }
  return names;
}

template <typename T>
std::vector<std::string> MultibodyPlant<T>::GetActuatorNames(
    ModelInstanceIndex model_instance, bool add_model_instance_prefix) const {
  DRAKE_MBP_THROW_IF_NOT_FINALIZED();
  std::vector<std::string> names(num_actuators(model_instance));
  std::vector<JointActuatorIndex> actuator_indices =
      GetJointActuatorIndices(model_instance);
  // The offset into the actuation array is the start of the first
  // mobilizer in the tree; here we just take the minimum.
  int offset = num_actuators();
  for (const auto& actuator_index : actuator_indices) {
    offset = std::min(offset, get_joint_actuator(actuator_index).input_start());
  }

  for (const auto& actuator_index : actuator_indices) {
    const JointActuator<T>& actuator = get_joint_actuator(actuator_index);
    // Sanity check: indices are in range.
    DRAKE_DEMAND(actuator.input_start() >= offset);
    DRAKE_DEMAND(actuator.input_start() - offset < ssize(names));

    const std::string prefix =
        add_model_instance_prefix
            ? fmt::format("{}_", GetModelInstanceName(model_instance))
            : "";
    // TODO(russt): Need to add actuator name suffix to JointActuator and loop
    // over actuator.num_inputs() if we ever actually support actuators with
    // multiple inputs.
    DRAKE_DEMAND(actuator.num_inputs() == 1);
    names[actuator.input_start() - offset] =
        fmt::format("{}{}", prefix, actuator.name());
  }
  return names;
}

template <typename T>
void MultibodyPlant<T>::EstimatePointContactParameters(
    double penetration_allowance) {
  // Default to Earth's gravity for this estimation.
  const UniformGravityFieldElement<T>& gravity = gravity_field();
  const double g = (!gravity.gravity_vector().isZero())
                       ? gravity.gravity_vector().norm()
                       : UniformGravityFieldElement<double>::kDefaultStrength;

  // TODO(amcastro-tri): Improve this heuristics in future PR's for when there
  // are several flying objects and fixed base robots (E.g.: manipulation
  // cases.)

  // The heuristic now is very simple. We should update it to:
  //  - Only scan free bodies for weight.
  //  - Consider an estimate of maximum velocities (context dependent).
  // Right now we are being very conservative and use the maximum mass in the
  // system.
  double mass = 0.0;
  for (BodyIndex body_index(0); body_index < num_bodies(); ++body_index) {
    const Body<T>& body = get_body(body_index);
    mass = std::max(mass, body.default_mass());
  }

  // For now, we use the model of a critically damped spring mass oscillator
  // to estimate these parameters: mẍ+cẋ+kx=mg
  // Notice however that normal forces are computed according to: fₙ=kx(1+dẋ)
  // which translate to a second order oscillator of the form:
  // mẍ+(kdx)ẋ+kx=mg
  // Therefore, for this more complex, non-linear, oscillator, we estimate the
  // damping constant d using a time scale related to the free oscillation
  // (omega below) and the requested penetration allowance as a length scale.

  // We first estimate the combined stiffness based on static equilibrium.
  const double combined_stiffness = mass * g / penetration_allowance;
  // Frequency associated with the combined_stiffness above.
  const double omega = sqrt(combined_stiffness / mass);

  // Estimated contact time scale. The relative velocity of objects coming into
  // contact goes to zero in this time scale.
  const double time_scale = 1.0 / omega;

  // Damping ratio for a critically damped model. We could allow users to set
  // this. Right now, critically damp the normal direction.
  // This corresponds to a non-penetraion constraint in the limit for
  // contact_penetration_allowance_ going to zero (no bounce off).
  const double damping_ratio = 1.0;
  // We form the dissipation (with units of 1/velocity) using dimensional
  // analysis. Thus we use 1/omega for the time scale and penetration_allowance
  // for the length scale. We then scale it by the damping ratio.
  const double dissipation = damping_ratio * time_scale / penetration_allowance;

  // Final parameters used in the penalty method:
  //
  // Before #13630 this method estimated an effective "combined" stiffness.
  // That is, penalty_method_contact_parameters_.geometry_stiffness (previously
  // called penalty_method_contact_parameters_.stiffness) was the desired
  // stiffness of the contact pair. Post #13630, the semantics of this variable
  // changes to "stiffness per contact geometry". Therefore, in order to
  // maintain backwards compatibility for sims run pre #13630, we include now a
  // factor of 2 so that when two geometries have the same stiffness, the
  // combined stiffness reduces to combined_stiffness.
  //
  // Stiffness in the penalty method is calculated as a combination of
  // individual stiffness parameters per geometry. The variable
  // `combined_stiffness` as calculated here is a combined stiffness, but
  // `penalty_method_contact_parameters_.geometry_stiffness` stores the
  // parameter for an individual geometry. Combined stiffness, for geometries
  // with individual stiffnesses k1 and k2 respectively, is defined as:
  //   Kc = (k1*k2) / (k1 + k2)
  // If we have a desired combined stiffness Kd (for two geometries with
  // default heuristically computed parameters), setting k1 = k2 = 2 * Kd
  // results in the correct combined stiffness:
  //   Kc = (2*Kd*2*Kd) / (2*Kd + 2*Kd) = Kd
  // Therefore we set the `geometry_stiffness` to 2*`combined_stiffness`.
  penalty_method_contact_parameters_.geometry_stiffness =
      2 * combined_stiffness;
  penalty_method_contact_parameters_.dissipation = dissipation;
  // The time scale can be requested to hint the integrator's time step.
  penalty_method_contact_parameters_.time_scale = time_scale;
}

template <typename T>
void MultibodyPlant<T>::CalcPointPairPenetrations(
    const systems::Context<T>& context,
    std::vector<PenetrationAsPointPair<T>>* output) const {
  this->ValidateContext(context);
  if (num_collision_geometries() > 0) {
    const auto& query_object = EvalGeometryQueryInput(context, __func__);
    *output = query_object.ComputePointPairPenetration();
  } else {
    output->clear();
  }
}

template<typename T>
void MultibodyPlant<T>::CopyContactResultsOutput(
    const systems::Context<T>& context,
    ContactResults<T>* contact_results) const {
  this->ValidateContext(context);

  // Guard against failure to acquire the geometry input deep in the call graph.
  ValidateGeometryInput(context, get_contact_results_output_port());

  DRAKE_DEMAND(contact_results != nullptr);
  *contact_results = EvalContactResults(context);
}

template <typename T>
void MultibodyPlant<T>::CalcContactResultsContinuous(
    const systems::Context<T>& context,
    ContactResults<T>* contact_results) const {
  this->ValidateContext(context);
  DRAKE_DEMAND(contact_results != nullptr);
  contact_results->Clear();
  contact_results->set_plant(this);
  if (num_collision_geometries() == 0) return;

  switch (contact_model_) {
    case ContactModel::kPoint:
      AppendContactResultsContinuousPointPair(context, contact_results);
      break;

    case ContactModel::kHydroelastic:
      AppendContactResultsContinuousHydroelastic(context, contact_results);
      break;

    case ContactModel::kHydroelasticWithFallback:
      // Simply merge the contributions of each contact representation.
      AppendContactResultsContinuousPointPair(context, contact_results);
      AppendContactResultsContinuousHydroelastic(context, contact_results);
      break;
  }
}

template <>
void MultibodyPlant<symbolic::Expression>::
    AppendContactResultsContinuousHydroelastic(
        const Context<symbolic::Expression>&,
        ContactResults<symbolic::Expression>*) const {
  throw std::logic_error(
      "This method doesn't support T = symbolic::Expression.");
}

template <typename T>
void MultibodyPlant<T>::AppendContactResultsContinuousHydroelastic(
    const systems::Context<T>& context,
    ContactResults<T>* contact_results) const {
  this->ValidateContext(context);
  DRAKE_DEMAND(contact_results != nullptr);
  DRAKE_DEMAND(contact_results->plant() == this);
  const internal::HydroelasticContactInfoAndBodySpatialForces<T>&
      contact_info_and_spatial_body_forces =
          EvalHydroelasticContactForces(context);
  for (const HydroelasticContactInfo<T>& contact_info :
       contact_info_and_spatial_body_forces.contact_info) {
    // Note: caching dependencies guarantee that the lifetime of contact_info is
    // valid for the lifetime of the contact results.
    contact_results->AddContactInfo(&contact_info);
  }
}

template <typename T>
void MultibodyPlant<T>::AppendContactResultsContinuousPointPair(
    const systems::Context<T>& context,
    ContactResults<T>* contact_results) const {
  this->ValidateContext(context);
  DRAKE_DEMAND(contact_results != nullptr);
  DRAKE_DEMAND(contact_results->plant() == this);

  const std::vector<PenetrationAsPointPair<T>>& point_pairs =
      EvalPointPairPenetrations(context);

  const internal::PositionKinematicsCache<T>& pc =
      EvalPositionKinematics(context);
  const internal::VelocityKinematicsCache<T>& vc =
      EvalVelocityKinematics(context);

  const geometry::QueryObject<T>& query_object =
      EvalGeometryQueryInput(context, __func__);
  const geometry::SceneGraphInspector<T>& inspector = query_object.inspector();

  for (size_t icontact = 0; icontact < point_pairs.size(); ++icontact) {
    const auto& pair = point_pairs[icontact];
    const GeometryId geometryA_id = pair.id_A;
    const GeometryId geometryB_id = pair.id_B;

    const BodyIndex bodyA_index = FindBodyByGeometryId(geometryA_id);
    const BodyIndex bodyB_index = FindBodyByGeometryId(geometryB_id);

    internal::BodyNodeIndex bodyA_node_index =
        get_body(bodyA_index).node_index();
    internal::BodyNodeIndex bodyB_node_index =
        get_body(bodyB_index).node_index();

    // Penetration depth, > 0 during pair.
    const T& x = pair.depth;
    DRAKE_ASSERT(x >= 0);
    const Vector3<T>& nhat_BA_W = pair.nhat_BA_W;
    const Vector3<T>& p_WCa = pair.p_WCa;
    const Vector3<T>& p_WCb = pair.p_WCb;

    // Contact point C.
    const Vector3<T> p_WC = 0.5 * (p_WCa + p_WCb);

    // Contact point position on body A.
    const Vector3<T>& p_WAo = pc.get_X_WB(bodyA_node_index).translation();
    const Vector3<T>& p_CoAo_W = p_WAo - p_WC;

    // Contact point position on body B.
    const Vector3<T>& p_WBo = pc.get_X_WB(bodyB_node_index).translation();
    const Vector3<T>& p_CoBo_W = p_WBo - p_WC;

    // Separation velocity, > 0  if objects separate.
    const Vector3<T> v_WAc =
        vc.get_V_WB(bodyA_node_index).Shift(-p_CoAo_W).translational();
    const Vector3<T> v_WBc =
        vc.get_V_WB(bodyB_node_index).Shift(-p_CoBo_W).translational();
    const Vector3<T> v_AcBc_W = v_WBc - v_WAc;

    // if xdot = vn > 0 ==> they are getting closer.
    const T vn = v_AcBc_W.dot(nhat_BA_W);

    // Magnitude of the normal force on body A at contact point C.
    const auto [kA, dA] = GetPointContactParameters(geometryA_id, inspector);
    const auto [kB, dB] = GetPointContactParameters(geometryB_id, inspector);
    const auto [k, d] = internal::CombinePointContactParameters(kA, kB, dA, dB);
    const T fn_AC = k * x * (1.0 + d * vn);

    // Acquire friction coefficients and combine them.
    const CoulombFriction<double>& geometryA_friction =
        GetCoulombFriction(geometryA_id, inspector);
    const CoulombFriction<double>& geometryB_friction =
        GetCoulombFriction(geometryB_id, inspector);
    const CoulombFriction<double> combined_friction =
        CalcContactFrictionFromSurfaceProperties(geometryA_friction,
                                                 geometryB_friction);

    if (fn_AC > 0) {
      // Normal force on body A, at C, expressed in W.
      const Vector3<T> fn_AC_W = fn_AC * nhat_BA_W;

      // Compute tangential velocity, that is, v_AcBc projected onto the tangent
      // plane with normal nhat_BA:
      const Vector3<T> vt_AcBc_W = v_AcBc_W - vn * nhat_BA_W;
      // Tangential speed (squared):
      const T vt_squared = vt_AcBc_W.squaredNorm();

      // Consider a value indistinguishable from zero if it is smaller
      // then 1e-14 and test against that value squared.
      const T kNonZeroSqd = 1e-14 * 1e-14;
      // Tangential friction force on A at C, expressed in W.
      Vector3<T> ft_AC_W = Vector3<T>::Zero();
      T slip_velocity = 0;
      if (vt_squared > kNonZeroSqd) {
        slip_velocity = sqrt(vt_squared);
        // Stribeck friction coefficient.
        const T mu_stribeck = friction_model_.ComputeFrictionCoefficient(
            slip_velocity, combined_friction);
        // Tangential direction.
        const Vector3<T> that_W = vt_AcBc_W / slip_velocity;

        // Magnitude of the friction force on A at C.
        const T ft_AC = mu_stribeck * fn_AC;
        ft_AC_W = ft_AC * that_W;
      }

      // Spatial force on body A at C, expressed in the world frame W.
      const SpatialForce<T> F_AC_W(Vector3<T>::Zero(), fn_AC_W + ft_AC_W);

      const Vector3<T> f_Bc_W = -F_AC_W.translational();
      contact_results->AddContactInfo(
          {bodyA_index, bodyB_index, f_Bc_W, p_WC, vn, slip_velocity, pair});
    }
  }
}

template <typename T>
void MultibodyPlant<T>::CalcContactResultsDiscrete(
    const systems::Context<T>& context,
    ContactResults<T>* contact_results) const {
  DRAKE_DEMAND(contact_results != nullptr);
  discrete_update_manager_->CalcContactResults(context, contact_results);
}

template <typename T>
void MultibodyPlant<T>::CalcAndAddContactForcesByPenaltyMethod(
    const systems::Context<T>& context,
    std::vector<SpatialForce<T>>* F_BBo_W_array) const {
  this->ValidateContext(context);
  DRAKE_DEMAND(F_BBo_W_array != nullptr);
  DRAKE_DEMAND(ssize(*F_BBo_W_array) == num_bodies());
  if (num_collision_geometries() == 0) return;

  const ContactResults<T>& contact_results = EvalContactResults(context);

  const internal::PositionKinematicsCache<T>& pc =
      EvalPositionKinematics(context);

  for (int pair_index = 0;
       pair_index < contact_results.num_point_pair_contacts(); ++pair_index) {
    const PointPairContactInfo<T>& contact_info =
        contact_results.point_pair_contact_info(pair_index);
    const PenetrationAsPointPair<T>& pair = contact_info.point_pair();

    const GeometryId geometryA_id = pair.id_A;
    const GeometryId geometryB_id = pair.id_B;

    const BodyIndex bodyA_index = FindBodyByGeometryId(geometryA_id);
    const BodyIndex bodyB_index = FindBodyByGeometryId(geometryB_id);

    internal::BodyNodeIndex bodyA_node_index =
        get_body(bodyA_index).node_index();
    internal::BodyNodeIndex bodyB_node_index =
        get_body(bodyB_index).node_index();

    // Contact point C.
    const Vector3<T> p_WC = contact_info.contact_point();

    // Contact point position on body A.
    const Vector3<T>& p_WAo = pc.get_X_WB(bodyA_node_index).translation();
    const Vector3<T>& p_CoAo_W = p_WAo - p_WC;

    // Contact point position on body B.
    const Vector3<T>& p_WBo = pc.get_X_WB(bodyB_node_index).translation();
    const Vector3<T>& p_CoBo_W = p_WBo - p_WC;

    const Vector3<T> f_Bc_W = contact_info.contact_force();
    const SpatialForce<T> F_AC_W(Vector3<T>::Zero(), -f_Bc_W);

    if (bodyA_index != world_index()) {
      // Spatial force on body A at Ao, expressed in W.
      const SpatialForce<T> F_AAo_W = F_AC_W.Shift(p_CoAo_W);
      F_BBo_W_array->at(bodyA_node_index) += F_AAo_W;
    }

    if (bodyB_index != world_index()) {
      // Spatial force on body B at Bo, expressed in W.
      const SpatialForce<T> F_BBo_W = -F_AC_W.Shift(p_CoBo_W);
      F_BBo_W_array->at(bodyB_node_index) += F_BBo_W;
    }
  }
}

template <>
void MultibodyPlant<symbolic::Expression>::CalcHydroelasticContactForces(
    const Context<symbolic::Expression>&,
    internal::HydroelasticContactInfoAndBodySpatialForces<
        symbolic::Expression>*) const {
  throw std::logic_error(
      "This method doesn't support T = symbolic::Expression.");
}

template <typename T>
void MultibodyPlant<T>::CalcHydroelasticContactForces(
    const Context<T>& context,
    internal::HydroelasticContactInfoAndBodySpatialForces<T>*
        contact_info_and_body_forces) const {
  this->ValidateContext(context);
  DRAKE_DEMAND(contact_info_and_body_forces != nullptr);

  std::vector<SpatialForce<T>>& F_BBo_W_array =
      contact_info_and_body_forces->F_BBo_W_array;
  DRAKE_DEMAND(ssize(F_BBo_W_array) == num_bodies());
  std::vector<HydroelasticContactInfo<T>>& contact_info =
      contact_info_and_body_forces->contact_info;

  // Initialize the body forces to zero.
  F_BBo_W_array.assign(num_bodies(), SpatialForce<T>::Zero());
  if (num_collision_geometries() == 0) return;

  const std::vector<ContactSurface<T>>& all_surfaces =
      EvalContactSurfaces(context);

  // Reserve memory here to keep from repeatedly allocating heap storage in the
  // loop below.
  contact_info.clear();
  contact_info.reserve(all_surfaces.size());

  internal::HydroelasticTractionCalculator<T> traction_calculator(
      friction_model_.stiction_tolerance());

  const auto& query_object = EvalGeometryQueryInput(context, __func__);
  const geometry::SceneGraphInspector<T>& inspector = query_object.inspector();

  for (const ContactSurface<T>& surface : all_surfaces) {
    const GeometryId geometryM_id = surface.id_M();
    const GeometryId geometryN_id = surface.id_N();

    const ProximityProperties* propM =
        inspector.GetProximityProperties(geometryM_id);
    const ProximityProperties* propN =
        inspector.GetProximityProperties(geometryM_id);
    DRAKE_DEMAND(propM != nullptr);
    DRAKE_DEMAND(propN != nullptr);
    DRAKE_THROW_UNLESS(propM->HasProperty(geometry::internal::kMaterialGroup,
                                          geometry::internal::kFriction));
    DRAKE_THROW_UNLESS(propN->HasProperty(geometry::internal::kMaterialGroup,
                                          geometry::internal::kFriction));

    const CoulombFriction<double>& geometryM_friction =
        propM->GetProperty<CoulombFriction<double>>(
            geometry::internal::kMaterialGroup, geometry::internal::kFriction);
    const CoulombFriction<double>& geometryN_friction =
        propN->GetProperty<CoulombFriction<double>>(
            geometry::internal::kMaterialGroup, geometry::internal::kFriction);

    // Compute combined friction coefficient.
    const CoulombFriction<double> combined_friction =
        CalcContactFrictionFromSurfaceProperties(geometryM_friction,
                                                 geometryN_friction);
    const double dynamic_friction = combined_friction.dynamic_friction();

    // Get the bodies that the two geometries are affixed to. We'll call these
    // A and B.
    const BodyIndex bodyA_index = FindBodyByGeometryId(geometryM_id);
    const BodyIndex bodyB_index = FindBodyByGeometryId(geometryN_id);
    const Body<T>& bodyA = get_body(bodyA_index);
    const Body<T>& bodyB = get_body(bodyB_index);

    // The poses and spatial velocities of bodies A and B.
    const RigidTransform<T>& X_WA = bodyA.EvalPoseInWorld(context);
    const RigidTransform<T>& X_WB = bodyB.EvalPoseInWorld(context);
    const SpatialVelocity<T>& V_WA = bodyA.EvalSpatialVelocityInWorld(context);
    const SpatialVelocity<T>& V_WB = bodyB.EvalSpatialVelocityInWorld(context);

    // Pack everything calculator needs.
    typename internal::HydroelasticTractionCalculator<T>::Data data(
        X_WA, X_WB, V_WA, V_WB, &surface);

    // Combined Hunt & Crossley dissipation.
    const hydroelastics::internal::HydroelasticEngine<T> hydroelastics_engine;
    const double dissipation = hydroelastics_engine.CalcCombinedDissipation(
        geometryM_id, geometryN_id, inspector);

    // Integrate the hydroelastic traction field over the contact surface.
    std::vector<HydroelasticQuadraturePointData<T>> traction_output;
    SpatialForce<T> F_Ac_W;
    traction_calculator.ComputeSpatialForcesAtCentroidFromHydroelasticModel(
        data, dissipation, dynamic_friction, &traction_output, &F_Ac_W);

    // Shift the traction at the centroid to tractions at the body origins.
    SpatialForce<T> F_Ao_W, F_Bo_W;
    traction_calculator.ShiftSpatialForcesAtCentroidToBodyOrigins(
        data, F_Ac_W, &F_Ao_W, &F_Bo_W);

    if (bodyA_index != world_index()) {
      F_BBo_W_array.at(bodyA.node_index()) += F_Ao_W;
    }

    if (bodyB_index != world_index()) {
      F_BBo_W_array.at(bodyB.node_index()) += F_Bo_W;
    }

    // Add the information for contact reporting.
    contact_info.emplace_back(&surface, F_Ac_W, std::move(traction_output));
  }
}

template <typename T>
void MultibodyPlant<T>::AddInForcesFromInputPorts(
    const drake::systems::Context<T>& context,
    MultibodyForces<T>* forces) const {
  this->ValidateContext(context);
  AddAppliedExternalGeneralizedForces(context, forces);
  AddAppliedExternalSpatialForces(context, forces);
  AddJointActuationForces(context, forces);
}

template<typename T>
void MultibodyPlant<T>::AddAppliedExternalGeneralizedForces(
    const systems::Context<T>& context, MultibodyForces<T>* forces) const {
  this->ValidateContext(context);
  // If there are applied generalized forces, add them in.
  const InputPort<T>& applied_generalized_force_input =
      this->get_input_port(applied_generalized_force_input_port_);
  if (applied_generalized_force_input.HasValue(context)) {
    const VectorX<T>& applied_generalized_force =
        applied_generalized_force_input.Eval(context);
    if (applied_generalized_force.hasNaN()) {
      throw std::runtime_error(
          "Detected NaN in applied generalized force input port.");
    }
    forces->mutable_generalized_forces() += applied_generalized_force;
  }
}

template <typename T>
void MultibodyPlant<T>::CalcGeneralizedForces(
    const systems::Context<T>& context, const MultibodyForces<T>& forces,
    VectorX<T>* generalized_forces) const {
  this->ValidateContext(context);
  DRAKE_THROW_UNLESS(forces.CheckHasRightSizeForModel(*this));
  DRAKE_THROW_UNLESS(generalized_forces != nullptr);
  generalized_forces->resize(num_velocities());
  // Heap allocate the necessary workspace.
  // TODO(amcastro-tri): Get rid of these heap allocations.
  std::vector<SpatialAcceleration<T>> A_scratch(num_bodies());
  std::vector<SpatialForce<T>> F_scratch(num_bodies());
  const VectorX<T> zero_vdot = VectorX<T>::Zero(num_velocities());
  // TODO(amcastro-tri): For performance, update this implementation to exclude
  // terms involving accelerations.
  const bool zero_velocities = true;
  internal_tree().CalcInverseDynamics(
      context, zero_vdot, forces.body_forces(), forces.generalized_forces(),
      zero_velocities, &A_scratch, &F_scratch, generalized_forces);
  *generalized_forces = -*generalized_forces;
}

template<typename T>
void MultibodyPlant<T>::AddAppliedExternalSpatialForces(
    const systems::Context<T>& context, MultibodyForces<T>* forces) const {
  // Get the mutable applied external spatial forces vector
  // (a.k.a., body force vector).
  this->ValidateContext(context);
  std::vector<SpatialForce<T>>& F_BBo_W_array = forces->mutable_body_forces();

  // Evaluate the input port; if it's not connected, return now.
  const auto* applied_input = this->template EvalInputValue<
      std::vector<ExternallyAppliedSpatialForce<T>>>(
          context, applied_spatial_force_input_port_);
  if (!applied_input)
    return;

  // Helper to throw a useful message if the input contains NaN.
  auto throw_if_contains_nan = [this](const ExternallyAppliedSpatialForce<T>&
                                          external_spatial_force) {
    const SpatialForce<T>& spatial_force = external_spatial_force.F_Bq_W;
    if (external_spatial_force.p_BoBq_B.hasNaN() ||
        spatial_force.rotational().hasNaN() ||
        spatial_force.translational().hasNaN()) {
      throw std::runtime_error(fmt::format(
          "Spatial force applied on body {} contains NaN.",
          internal_tree().get_body(external_spatial_force.body_index).name()));
    }
  };
  // Loop over all forces.
  for (const auto& force_structure : *applied_input) {
    throw_if_contains_nan(force_structure);
    const BodyIndex body_index = force_structure.body_index;
    const Body<T>& body = get_body(body_index);
    const auto body_node_index = body.node_index();

    // Get the pose for this body in the world frame.
    const RigidTransform<T>& X_WB = EvalBodyPoseInWorld(context, body);

    // Get the position vector from the body origin (Bo) to the point of
    // force application (Bq), expressed in the world frame (W).
    const Vector3<T> p_BoBq_W = X_WB.rotation() * force_structure.p_BoBq_B;

    // Shift the spatial force from Bq to Bo.
    F_BBo_W_array[body_node_index] += force_structure.F_Bq_W.Shift(-p_BoBq_W);
  }
}

template<typename T>
void MultibodyPlant<T>::AddJointActuationForces(
    const systems::Context<T>& context, MultibodyForces<T>* forces) const {
  this->ValidateContext(context);
  DRAKE_DEMAND(forces != nullptr);
  if (num_actuators() > 0) {
    const VectorX<T> u = AssembleActuationInput(context);
    for (JointActuatorIndex actuator_index(0);
         actuator_index < num_actuators(); ++actuator_index) {
      const JointActuator<T>& actuator =
          get_joint_actuator(actuator_index);
      // We only support actuators on single dof joints for now.
      DRAKE_DEMAND(actuator.joint().num_velocities() == 1);
      for (int joint_dof = 0;
           joint_dof < actuator.joint().num_velocities(); ++joint_dof) {
        actuator.AddInOneForce(context, joint_dof, u[actuator_index], forces);
      }
    }
  }
}

template<typename T>
void MultibodyPlant<T>::AddJointLimitsPenaltyForces(
    const systems::Context<T>& context, MultibodyForces<T>* forces) const {
  this->ValidateContext(context);
  DRAKE_THROW_UNLESS(is_discrete());
  DRAKE_DEMAND(forces != nullptr);

  auto CalcPenaltyForce = [](
      double lower_limit, double upper_limit, double stiffness, double damping,
      const T& q, const T& v) {
    DRAKE_DEMAND(lower_limit <= upper_limit);
    DRAKE_DEMAND(stiffness >= 0);
    DRAKE_DEMAND(damping >= 0);

    if (q > upper_limit) {
      const T delta_q = q - upper_limit;
      const T limit_force = -stiffness * delta_q - damping * v;
      using std::min;  // Needed for ADL.
      return min(limit_force, 0.);
    } else if (q < lower_limit) {
      const T delta_q = q - lower_limit;
      const T limit_force = -stiffness * delta_q - damping * v;
      using std::max;  // Needed for ADL.
      return max(limit_force, 0.);
    }
    return T(0.0);
  };

  for (size_t index = 0;
       index < joint_limits_parameters_.joints_with_limits.size(); ++index) {
    const JointIndex joint_index =
        joint_limits_parameters_.joints_with_limits[index];
    const double lower_limit = joint_limits_parameters_.lower_limit[index];
    const double upper_limit = joint_limits_parameters_.upper_limit[index];
    const double stiffness = joint_limits_parameters_.stiffness[index];
    const double damping = joint_limits_parameters_.damping[index];
    const Joint<T>& joint = get_joint(joint_index);

    const T& q = joint.GetOnePosition(context);
    const T& v = joint.GetOneVelocity(context);

    const T penalty_force = CalcPenaltyForce(
        lower_limit, upper_limit, stiffness, damping, q, v);

    joint.AddInOneForce(context, 0, penalty_force, forces);
  }
}

template<typename T>
VectorX<T> MultibodyPlant<T>::AssembleActuationInput(
    const systems::Context<T>& context) const {
  this->ValidateContext(context);

  // Assemble the vector from the model instance input ports.
  // TODO(sherm1) Heap allocation here. Get rid of it.
  VectorX<T> actuation_input(num_actuated_dofs());

  const auto& actuation_port = this->get_input_port(actuation_port_);
  const ModelInstanceIndex first_non_world_index(1);
  if (actuation_port.HasValue(context)) {
    // The port for all instances and the actuation ports for individual
    // instances should not be connected at the same time.
    for (ModelInstanceIndex model_instance_index(first_non_world_index);
         model_instance_index < num_model_instances(); ++model_instance_index) {
      const auto& per_instance_actuation_port =
          this->get_input_port(instance_actuation_ports_[model_instance_index]);
      if (per_instance_actuation_port.HasValue(context)) {
        throw std::logic_error(fmt::format(
            "Actuation input port for model instance {} and the "
            "actuation port for all instances are both connected. At most "
            "one of these ports should be connected.",
            GetModelInstanceName(model_instance_index)));
      }
    }
    // TODO(xuchenhan-tri): It'd be nice to avoid the copy here.
    actuation_input = actuation_port.Eval(context);
    if (actuation_input.hasNaN()) {
      throw std::runtime_error(
          "Detected NaN in the actuation input port for all instances.");
    }
    DRAKE_ASSERT(actuation_input.size() == num_actuated_dofs());
  } else {
    int u_offset = 0;
    for (ModelInstanceIndex model_instance_index(first_non_world_index);
         model_instance_index < num_model_instances(); ++model_instance_index) {
      // Ignore the port if the model instance has no actuated DoFs.
      const int instance_num_dofs = num_actuated_dofs(model_instance_index);
      if (instance_num_dofs == 0) continue;

      const auto& input_port =
          this->get_input_port(instance_actuation_ports_[model_instance_index]);
      if (!input_port.HasValue(context)) {
        throw std::logic_error(fmt::format("Actuation input port for model "
            "instance {} must be connected.",
            GetModelInstanceName(model_instance_index)));
      }
      const auto& u_instance = input_port.Eval(context);

      if (u_instance.hasNaN()) {
        throw std::runtime_error(
            fmt::format("Actuation input port for model "
                        "instance {} contains NaN.",
                        GetModelInstanceName(model_instance_index)));
      }
      actuation_input.segment(u_offset, instance_num_dofs) = u_instance;
      u_offset += instance_num_dofs;
    }
    DRAKE_ASSERT(u_offset == num_actuated_dofs());
  }
  return actuation_input;
}

template <typename T>
void MultibodyPlant<T>::CalcContactSurfaces(
    const drake::systems::Context<T>& context,
    std::vector<ContactSurface<T>>* contact_surfaces) const {
  this->ValidateContext(context);
  DRAKE_DEMAND(contact_surfaces != nullptr);

  const auto& query_object = EvalGeometryQueryInput(context, __func__);

  *contact_surfaces = query_object.ComputeContactSurfaces(
      get_contact_surface_representation());
}

template <>
void MultibodyPlant<symbolic::Expression>::CalcContactSurfaces(
    const Context<symbolic::Expression>&,
    std::vector<geometry::ContactSurface<symbolic::Expression>>*) const {
  throw std::logic_error(
      "This method doesn't support T = symbolic::Expression.");
}

template <typename T>
void MultibodyPlant<T>::CalcHydroelasticWithFallback(
    const drake::systems::Context<T>& context,
    internal::HydroelasticFallbackCacheData<T>* data) const {
  this->ValidateContext(context);
  DRAKE_DEMAND(data != nullptr);

  if (num_collision_geometries() > 0) {
    const auto &query_object = EvalGeometryQueryInput(context, __func__);
    data->contact_surfaces.clear();
    data->point_pairs.clear();

    query_object.ComputeContactSurfacesWithFallback(
        get_contact_surface_representation(), &data->contact_surfaces,
        &data->point_pairs);
  }
}

template <>
void MultibodyPlant<symbolic::Expression>::CalcHydroelasticWithFallback(
    const drake::systems::Context<symbolic::Expression>&,
    internal::HydroelasticFallbackCacheData<symbolic::Expression>*) const {
  // TODO(SeanCurtis-TRI): Special case the AutoDiff scalar such that it works
  //  as long as there are no collisions -- akin to CalcPointPairPenetrations().
  throw std::domain_error(
      fmt::format("This method doesn't support T = {}.",
                  NiceTypeName::Get<symbolic::Expression>()));
}

template <typename T>
void MultibodyPlant<T>::CalcJointLockingIndices(
    const systems::Context<T>& context,
    std::vector<int>* unlocked_velocity_indices) const {
  DRAKE_DEMAND(unlocked_velocity_indices != nullptr);
  auto& indices = *unlocked_velocity_indices;
  indices.resize(num_velocities());

  int unlocked_cursor = 0;
  for (JointIndex joint_index(0); joint_index < num_joints(); ++joint_index) {
    const Joint<T>& joint = get_joint(joint_index);
    if (!joint.is_locked(context)) {
      for (int k = 0; k < joint.num_velocities(); ++k) {
        indices[unlocked_cursor++] = joint.velocity_start() + k;
      }
    }
  }

  DRAKE_ASSERT(unlocked_cursor <= num_velocities());

  // Use size to indicate exactly how many velocities are unlocked.
  indices.resize(unlocked_cursor);
  // Sort the unlocked indices to keep the original DOF ordering established by
  // the plant stable.
  std::sort(indices.begin(), indices.end());
  internal::DemandIndicesValid(indices, num_velocities());
  DRAKE_DEMAND(ssize(indices) == unlocked_cursor);
}

template <typename T>
void MultibodyPlant<T>::CalcGeneralizedContactForcesContinuous(
    const Context<T>& context, VectorX<T>* tau_contact) const {
  this->ValidateContext(context);
  DRAKE_DEMAND(tau_contact != nullptr);
  DRAKE_DEMAND(tau_contact->size() == num_velocities());
  DRAKE_DEMAND(!is_discrete());
  const int nv = this->num_velocities();

  // Early exit if there are no contact forces.
  tau_contact->setZero();
  if (num_collision_geometries() == 0) return;

  // We will alias this zero vector to serve both as zero-valued generalized
  // accelerations and zero-valued externally applied generalized forces.
  const VectorX<T> zero = VectorX<T>::Zero(nv);
  const VectorX<T>& zero_vdot = zero;
  const VectorX<T>& tau_array = zero;

  // Get the spatial forces.
  const std::vector<SpatialForce<T>>& Fcontact_BBo_W_array =
      EvalSpatialContactForcesContinuous(context);

  // Bodies' accelerations and inboard mobilizer reaction forces, respectively,
  // ordered by BodyNodeIndex and required as output arguments for
  // CalcInverseDynamics() below but otherwise not used by this method.
  std::vector<SpatialAcceleration<T>> A_WB_array(num_bodies());
  std::vector<SpatialForce<T>> F_BMo_W_array(num_bodies());

  // With vdot = 0, this computes:
  //   tau_contact = - ∑ J_WBᵀ(q) Fcontact_Bo_W.
  internal_tree().CalcInverseDynamics(
      context, zero_vdot, Fcontact_BBo_W_array, tau_array,
      true /* Do not compute velocity-dependent terms */,
      &A_WB_array, &F_BMo_W_array, tau_contact);

  // Per above, tau_contact must be negated to get ∑ J_WBᵀ(q) Fcontact_Bo_W.
  (*tau_contact) = -(*tau_contact);
}

template <typename T>
void MultibodyPlant<T>::CalcSpatialContactForcesContinuous(
      const drake::systems::Context<T>& context,
      std::vector<SpatialForce<T>>* F_BBo_W_array) const {
  this->ValidateContext(context);
  DRAKE_DEMAND(F_BBo_W_array != nullptr);
  DRAKE_DEMAND(ssize(*F_BBo_W_array) == num_bodies());
  DRAKE_DEMAND(!is_discrete());

  // Forces can accumulate into F_BBo_W_array; initialize it to zero first.
  std::fill(F_BBo_W_array->begin(), F_BBo_W_array->end(),
            SpatialForce<T>::Zero());

  CalcAndAddSpatialContactForcesContinuous(context, F_BBo_W_array);
}

template <typename T>
void MultibodyPlant<T>::CalcAndAddSpatialContactForcesContinuous(
      const drake::systems::Context<T>& context,
      std::vector<SpatialForce<T>>* F_BBo_W_array) const {
  this->ValidateContext(context);
  DRAKE_DEMAND(F_BBo_W_array != nullptr);
  DRAKE_DEMAND(ssize(*F_BBo_W_array) == num_bodies());
  DRAKE_DEMAND(!is_discrete());

  // Early exit if there are no contact forces.
  if (num_collision_geometries() == 0) return;

  // Note: we don't need to know the applied forces here because we use a
  // regularized friction model whose forces depend only on the current state; a
  // constraint based friction model would require accounting for the applied
  // forces.

  // Compute the spatial forces on each body from contact.
  switch (contact_model_) {
    case ContactModel::kPoint:
      // Note: consider caching the results from the following method (in which
      // case we would also want to introduce the Eval... naming convention for
      // the method).
      CalcAndAddContactForcesByPenaltyMethod(context, &(*F_BBo_W_array));
      break;

    case ContactModel::kHydroelastic:
      *F_BBo_W_array = EvalHydroelasticContactForces(context).F_BBo_W_array;
      break;

    case ContactModel::kHydroelasticWithFallback:
      // Combine the point-penalty forces with the contact surface forces.
      CalcAndAddContactForcesByPenaltyMethod(context, &(*F_BBo_W_array));
      const std::vector<SpatialForce<T>>& Fhydro_BBo_W_all =
          EvalHydroelasticContactForces(context).F_BBo_W_array;
      DRAKE_DEMAND(F_BBo_W_array->size() == Fhydro_BBo_W_all.size());
      for (int i = 0; i < ssize(Fhydro_BBo_W_all); ++i) {
        // Both sets of forces are applied to the body's origins and expressed
        // in frame W. They should simply sum.
        (*F_BBo_W_array)[i] += Fhydro_BBo_W_all[i];
      }
      break;
  }
}

template <typename T>
void MultibodyPlant<T>::CalcNonContactForces(
    const drake::systems::Context<T>& context,
    bool discrete,
    MultibodyForces<T>* forces) const {
  this->ValidateContext(context);
  DRAKE_DEMAND(forces != nullptr);
  DRAKE_DEMAND(forces->CheckHasRightSizeForModel(*this));

  const ScopeExit guard = ThrowIfNonContactForceInProgress(context);

  // Compute forces applied through force elements. Note that this resets
  // forces to empty so must come first.
  CalcForceElementsContribution(context, forces);

  AddInForcesFromInputPorts(context, forces);

  // Only discrete models support joint limits.
  if (discrete) {
    AddJointLimitsPenaltyForces(context, forces);
  } else {
    auto& warning = joint_limits_parameters_.pending_warning_message;
    if (!warning.empty()) {
      drake::log()->warn(warning);
      warning.clear();
    }
  }
}
template <typename T>
ScopeExit MultibodyPlant<T>::ThrowIfNonContactForceInProgress(
    const systems::Context<T>& context) const {
  // To overcame issue #12786, we use this additional cache entry
  // to detect algebraic loops.
  systems::CacheEntryValue& value =
      this->get_cache_entry(
              cache_indexes_.non_contact_forces_evaluation_in_progress)
          .get_mutable_cache_entry_value(context);
  bool& evaluation_in_progress = value.GetMutableValueOrThrow<bool>();
  if (evaluation_in_progress) {
    const char* error_message =
        "Algebraic loop detected. This situation is caused when connecting "
        "the input of your MultibodyPlant to the output of a feedback system "
        "which is an algebraic function of a feedthrough output of the "
        "plant. Ways to remedy this: 1. Revisit the model for your feedback "
        "system. Consider if its output can be written in terms of other "
        "inputs. 2. Break the algebraic loop by adding state to the "
        "controller, typically to 'remember' a previous input. 3. Break the "
        "algebraic loop by adding a zero-order hold system between the "
        "output of the plant and your feedback system. This effectively "
        "delays the input signal to the controller.";
    throw std::runtime_error(error_message);
  }
  // Mark the start of the computation. If within an algebraic
  // loop, pulling from the plant's input ports during the
  // computation will trigger the recursive evaluation of this
  // method and the exception above will be thrown.
  evaluation_in_progress = true;
  // If the exception above is triggered, we will leave this method and the
  // computation will no longer be "in progress". We use a scoped guard so
  // that we have a chance to mark it as such when we leave this scope.
  return ScopeExit(
      [&evaluation_in_progress]() { evaluation_in_progress = false; });
}

template <typename T>
void MultibodyPlant<T>::AddInForcesContinuous(
    const systems::Context<T>& context, MultibodyForces<T>* forces) const {
  this->ValidateContext(context);

  // Guard against failure to acquire the geometry input deep in the call graph.
  ValidateGeometryInput(
      context, "You've tried evaluating time derivatives or their residuals.");

  // Forces from MultibodyTree elements are handled in MultibodyTreeSystem;
  // we need only handle MultibodyPlant-specific forces here.
  AddInForcesFromInputPorts(context, forces);

  // Add the contribution of contact forces.
  std::vector<SpatialForce<T>>& Fapp_BBo_W_array =
      forces->mutable_body_forces();
  const std::vector<SpatialForce<T>>& Fcontact_BBo_W_array =
      EvalSpatialContactForcesContinuous(context);
  for (int i = 0; i < ssize(Fapp_BBo_W_array); ++i)
    Fapp_BBo_W_array[i] += Fcontact_BBo_W_array[i];
}

template <typename T>
void MultibodyPlant<T>::DoCalcForwardDynamicsDiscrete(
    const drake::systems::Context<T>& context0,
    AccelerationKinematicsCache<T>* ac) const {
  this->ValidateContext(context0);
  DRAKE_DEMAND(ac != nullptr);
  DRAKE_DEMAND(is_discrete());

  // Guard against failure to acquire the geometry input deep in the call graph.
  ValidateGeometryInput(
      context0, "You've tried evaluating discrete forward dynamics.");

  DRAKE_DEMAND(discrete_update_manager_ != nullptr);
  discrete_update_manager_->CalcAccelerationKinematicsCache(context0, ac);
}

template<typename T>
systems::EventStatus MultibodyPlant<T>::CalcDiscreteStep(
    const systems::Context<T>& context0,
    systems::DiscreteValues<T>* updates) const {
  this->ValidateContext(context0);

  // TODO(amcastro-tri): remove the entirety of the code we are bypassing here.
  // This requires one of our custom managers to become the default
  // MultibodyPlant manager.
  if (discrete_update_manager_) {
    discrete_update_manager_->CalcDiscreteValues(context0, updates);
    return systems::EventStatus::Succeeded();
  }

  // Get the system state as raw Eigen vectors
  // (solution at the previous time step).
  auto x0 = context0.get_discrete_state(0).get_value();
  VectorX<T> q0 = x0.topRows(this->num_positions());
  VectorX<T> v0 = x0.bottomRows(this->num_velocities());

  // For a discrete model this evaluates vdot = (v_next - v0)/time_step() and
  // includes contact forces.
  const VectorX<T>& vdot = this->EvalForwardDynamics(context0).get_vdot();

  // TODO(amcastro-tri): Consider replacing this by:
  //   const VectorX<T>& v_next = solver_results.v_next;
  // to avoid additional vector operations.
  const VectorX<T>& v_next = v0 + time_step() * vdot;

  VectorX<T> qdot_next(this->num_positions());
  MapVelocityToQDot(context0, v_next, &qdot_next);
  VectorX<T> q_next = q0 + time_step() * qdot_next;

  VectorX<T> x_next(this->num_multibody_states());
  x_next << q_next, v_next;
  updates->set_value(0, x_next);

  return systems::EventStatus::Succeeded();
}

template<typename T>
void MultibodyPlant<T>::DeclareStateCacheAndPorts() {
  // The model must be finalized.
  DRAKE_DEMAND(this->is_finalized());

  if (is_discrete()) {
    this->DeclarePeriodicDiscreteUpdateEvent(
        time_step_, 0.0, &MultibodyPlant<T>::CalcDiscreteStep);

    // Also permit triggering a step via a Forced update.
    this->DeclareForcedDiscreteUpdateEvent(
        &MultibodyPlant<T>::CalcDiscreteStep);
  }

  DeclareCacheEntries();

  // Declare per model instance actuation ports.
  int num_actuated_instances = 0;
  ModelInstanceIndex last_actuated_instance;
  instance_actuation_ports_.resize(num_model_instances());
  for (ModelInstanceIndex model_instance_index(0);
       model_instance_index < num_model_instances(); ++model_instance_index) {
    const int instance_num_dofs = num_actuated_dofs(model_instance_index);
    if (instance_num_dofs > 0) {
      ++num_actuated_instances;
      last_actuated_instance = model_instance_index;
    }
    instance_actuation_ports_[model_instance_index] =
        this->DeclareVectorInputPort(
                GetModelInstanceName(model_instance_index) + "_actuation",
                instance_num_dofs)
            .get_index();
  }
  actuation_port_ =
      this->DeclareVectorInputPort("actuation", num_actuated_dofs())
          .get_index();

  // Declare the generalized force input port.
  applied_generalized_force_input_port_ =
      this->DeclareVectorInputPort("applied_generalized_force",
                                   num_velocities())
          .get_index();

  // Declare applied spatial force input force port.
  applied_spatial_force_input_port_ = this->DeclareAbstractInputPort(
        "applied_spatial_force",
        Value<std::vector<ExternallyAppliedSpatialForce<T>>>()).get_index();

  // Declare one output port for the entire state vector.
  state_output_port_ =
      this->DeclareVectorOutputPort("state", num_multibody_states(),
                                    &MultibodyPlant::CopyMultibodyStateOut,
                                    {this->all_state_ticket()})
          .get_index();

  // Declare the output port for the poses of all bodies in the world.
  body_poses_port_ =
      this->DeclareAbstractOutputPort(
              "body_poses", std::vector<math::RigidTransform<T>>(num_bodies()),
              &MultibodyPlant<T>::CalcBodyPosesOutput,
              {this->configuration_ticket()})
          .get_index();

  // Declare the output port for the spatial velocities of all bodies in the
  // world.
  body_spatial_velocities_port_ =
      this->DeclareAbstractOutputPort(
              "spatial_velocities",
              std::vector<SpatialVelocity<T>>(num_bodies()),
              &MultibodyPlant<T>::CalcBodySpatialVelocitiesOutput,
              {this->kinematics_ticket()})
          .get_index();

  // Declare the output port for the spatial accelerations of all bodies in the
  // world.
  body_spatial_accelerations_port_ =
      this->DeclareAbstractOutputPort(
              "spatial_accelerations",
              std::vector<SpatialAcceleration<T>>(num_bodies()),
              &MultibodyPlant<T>::CalcBodySpatialAccelerationsOutput,
              // Accelerations depend on both state and inputs.
              // All sources include: time, accuracy, state, input ports, and
              // parameters.
              {this->all_sources_ticket()})
          .get_index();

  // Declare one output port for the entire generalized acceleration vector
  // vdot (length is nv).
  generalized_acceleration_output_port_ =
      this->DeclareVectorOutputPort(
              "generalized_acceleration", num_velocities(),
              [this](const systems::Context<T>& context,
                     systems::BasicVector<T>* result) {
                result->SetFromVector(
                    this->EvalForwardDynamics(context).get_vdot());
              },
              {this->acceleration_kinematics_cache_entry().ticket()})
          .get_index();

  // Declare per model instance state and acceleration output ports.
  instance_state_output_ports_.resize(num_model_instances());
  instance_generalized_acceleration_output_ports_.resize(num_model_instances());
  for (ModelInstanceIndex model_instance_index(0);
       model_instance_index < num_model_instances(); ++model_instance_index) {
    const std::string& instance_name =
        GetModelInstanceName(model_instance_index);

    const int instance_num_states =  // Might be zero.
        num_multibody_states(model_instance_index);
    auto copy_instance_state_out = [this, model_instance_index](
        const Context<T>& context, BasicVector<T>* result) {
      this->CopyMultibodyStateOut(model_instance_index, context, result);
    };
    instance_state_output_ports_[model_instance_index] =
        this->DeclareVectorOutputPort(
                instance_name + "_state", instance_num_states,
                copy_instance_state_out, {this->all_state_ticket()})
            .get_index();

    const int instance_num_velocities =  // Might be zero.
        num_velocities(model_instance_index);
    instance_generalized_acceleration_output_ports_[model_instance_index] =
        this->DeclareVectorOutputPort(
                instance_name + "_generalized_acceleration",
                instance_num_velocities,
                [this, model_instance_index](const systems::Context<T>& context,
                                             systems::BasicVector<T>* result) {
                  const auto& vdot =
                      this->EvalForwardDynamics(context).get_vdot();
                  result->SetFromVector(
                      this->GetVelocitiesFromArray(model_instance_index, vdot));
                },
                {this->acceleration_kinematics_cache_entry().ticket()})
            .get_index();
  }

  // Declare per model instance output port of generalized contact forces.
  instance_generalized_contact_forces_output_ports_.resize(
      num_model_instances());
  for (ModelInstanceIndex model_instance_index(0);
       model_instance_index < num_model_instances(); ++model_instance_index) {
    const int instance_num_velocities = num_velocities(model_instance_index);

    if (is_discrete()) {
      auto calc = [this, model_instance_index](
                      const systems::Context<T>& context,
                      systems::BasicVector<T>* result) {
        // Guard against failure to acquire the geometry input deep in the call
        // graph.
        ValidateGeometryInput(
            context,
            get_generalized_contact_forces_output_port(model_instance_index));

        DRAKE_DEMAND(discrete_update_manager_ != nullptr);
        const contact_solvers::internal::ContactSolverResults<T>&
            solver_results =
                discrete_update_manager_->EvalContactSolverResults(context);
        this->CopyGeneralizedContactForcesOut(solver_results,
                                              model_instance_index, result);
      };
      instance_generalized_contact_forces_output_ports_[model_instance_index] =
          this->DeclareVectorOutputPort(
                  GetModelInstanceName(model_instance_index) +
                      "_generalized_contact_forces",
                  instance_num_velocities, calc,
                  {systems::System<T>::xd_ticket(),
                   systems::System<T>::all_parameters_ticket()})
              .get_index();
    } else {
      const auto& generalized_contact_forces_continuous_cache_entry =
          this->get_cache_entry(
              cache_indexes_.generalized_contact_forces_continuous);
      auto calc = [this, model_instance_index](
                      const systems::Context<T>& context,
                      systems::BasicVector<T>* result) {
        // Guard against failure to acquire the geometry input deep in the call
        // graph.
        ValidateGeometryInput(
            context,
            get_generalized_contact_forces_output_port(model_instance_index));

        result->SetFromVector(GetVelocitiesFromArray(
            model_instance_index,
            EvalGeneralizedContactForcesContinuous(context)));
      };
      instance_generalized_contact_forces_output_ports_[model_instance_index] =
          this->DeclareVectorOutputPort(
                  GetModelInstanceName(model_instance_index) +
                      "_generalized_contact_forces",
                  instance_num_velocities, calc,
                  {generalized_contact_forces_continuous_cache_entry.ticket()})
              .get_index();
    }
  }

  // Joint reaction forces are a function of accelerations, which in turn depend
  // on both state and inputs.
  reaction_forces_port_ =
      this->DeclareAbstractOutputPort(
              "reaction_forces", std::vector<SpatialForce<T>>(num_joints()),
              &MultibodyPlant<T>::CalcReactionForces,
              {this->acceleration_kinematics_cache_entry().ticket()})
          .get_index();

  // Contact results output port.
  const auto& contact_results_cache_entry =
      this->get_cache_entry(cache_indexes_.contact_results);
  contact_results_port_ = this->DeclareAbstractOutputPort(
                                  "contact_results", ContactResults<T>(),
                                  &MultibodyPlant<T>::CopyContactResultsOutput,
                                  {contact_results_cache_entry.ticket()})
                              .get_index();

  // See ThrowIfNonContactForceInProgress().
  const auto& non_contact_forces_evaluation_in_progress =
      this->DeclareCacheEntry(
          "Evaluation of non-contact forces and accelerations is in progress.",
          // N.B. This flag is set to true only when the computation is in
          // progress. Therefore its default value is `false`.
          systems::ValueProducer(false, &systems::ValueProducer::NoopCalc),
          {systems::System<T>::nothing_ticket()});
  cache_indexes_.non_contact_forces_evaluation_in_progress =
      non_contact_forces_evaluation_in_progress.cache_index();

  // Let external model managers declare their state, cache and ports in
  // `this` MultibodyPlant.
  for (auto& physical_model : physical_models_) {
    physical_model->DeclareSystemResources(this);
  }
}

template <typename T>
void MultibodyPlant<T>::DeclareCacheEntries() {
  DRAKE_DEMAND(this->is_finalized());

  // TODO(joemasterjohn): Create more granular parameter tickets for finer
  // control over cache dependencies on parameters. For example,
  // all_rigid_body_parameters, etc.

  // TODO(SeanCurtis-TRI): When SG caches the results of these queries itself,
  //  (https://github.com/RobotLocomotion/drake/issues/12767), remove these
  //  cache entries.
  auto& hydro_point_cache_entry = this->DeclareCacheEntry(
      std::string("Hydroelastic contact with point-pair fallback"),
      &MultibodyPlant::CalcHydroelasticWithFallback,
      {this->configuration_ticket()});
  cache_indexes_.hydro_fallback = hydro_point_cache_entry.cache_index();

  // Cache entry for point contact queries.
  auto& point_pairs_cache_entry = this->DeclareCacheEntry(
      std::string("Point pair penetrations."),
      &MultibodyPlant<T>::CalcPointPairPenetrations,
      {this->configuration_ticket()});
  cache_indexes_.point_pairs = point_pairs_cache_entry.cache_index();

  // Cache entry for hydroelastic contact surfaces.
  auto& contact_surfaces_cache_entry = this->DeclareCacheEntry(
      std::string("Hydroelastic contact surfaces."),
      &MultibodyPlant<T>::CalcContactSurfaces,
      {this->configuration_ticket()});
  cache_indexes_.contact_surfaces = contact_surfaces_cache_entry.cache_index();

  // Cache entry for spatial forces and contact info due to hydroelastic
  // contact.
  const bool use_hydroelastic =
      contact_model_ == ContactModel::kHydroelastic ||
      contact_model_ == ContactModel::kHydroelasticWithFallback;
  if (use_hydroelastic) {
    auto& contact_info_and_body_spatial_forces_cache_entry =
        this->DeclareCacheEntry(
            std::string("Hydroelastic contact info and body spatial forces."),
            internal::HydroelasticContactInfoAndBodySpatialForces<T>(
                this->num_bodies()),
            &MultibodyPlant<T>::CalcHydroelasticContactForces,
            // Compliant contact forces due to hydroelastics with Hunt &
            // Crosseley are function of the kinematic variables q & v only.
            {this->kinematics_ticket(), this->all_parameters_ticket()});
    cache_indexes_.contact_info_and_body_spatial_forces =
        contact_info_and_body_spatial_forces_cache_entry.cache_index();
  }

  // Cache contact results.
  // In discrete mode contact forces computation requires to advance the system
  // from step n to n+1. Therefore they are a function of state and input.
  // In continuous mode contact forces are simply a function of state.
  std::set<systems::DependencyTicket> dependency_ticket = [this,
                                                           use_hydroelastic]() {
    std::set<systems::DependencyTicket> tickets;
    if (is_discrete()) {
      tickets.insert(systems::System<T>::xd_ticket());
      tickets.insert(systems::System<T>::all_parameters_ticket());
    } else {
      tickets.insert(this->kinematics_ticket());
      if (use_hydroelastic) {
        tickets.insert(this->cache_entry_ticket(
            cache_indexes_.contact_info_and_body_spatial_forces));
      }
    }
    tickets.insert(this->all_parameters_ticket());

    return tickets;
  }();
  auto& contact_results_cache_entry = this->DeclareCacheEntry(
      std::string("Contact results."),
      is_discrete() ?
          &MultibodyPlant<T>::CalcContactResultsDiscrete :
          &MultibodyPlant<T>::CalcContactResultsContinuous,
      {dependency_ticket});
  cache_indexes_.contact_results = contact_results_cache_entry.cache_index();

  // Cache spatial continuous contact forces.
  auto& spatial_contact_forces_continuous_cache_entry = this->DeclareCacheEntry(
      "Spatial contact forces (continuous).",
      std::vector<SpatialForce<T>>(num_bodies()),
      &MultibodyPlant::CalcSpatialContactForcesContinuous,
      {this->kinematics_ticket(), this->all_parameters_ticket()});
  cache_indexes_.spatial_contact_forces_continuous =
      spatial_contact_forces_continuous_cache_entry.cache_index();

  // Cache generalized continuous contact forces.
  auto& generalized_contact_forces_continuous_cache_entry =
      this->DeclareCacheEntry(
          "Generalized contact forces (continuous).",
          VectorX<T>(num_velocities()),
          &MultibodyPlant::CalcGeneralizedContactForcesContinuous,
          {this->cache_entry_ticket(
               cache_indexes_.spatial_contact_forces_continuous),
           this->all_parameters_ticket()});
  cache_indexes_.generalized_contact_forces_continuous =
      generalized_contact_forces_continuous_cache_entry.cache_index();

  // Cache joint locking indices.
  const auto& joint_locking_data_cache_entry =
      this->DeclareCacheEntry("Joint Locking Indices.", std::vector<int>(),
                              &MultibodyPlant::CalcJointLockingIndices,
                              {this->all_parameters_ticket()});
  cache_indexes_.joint_locking_data =
      joint_locking_data_cache_entry.cache_index();
}

template <typename T>
void MultibodyPlant<T>::CopyMultibodyStateOut(
    const Context<T>& context, BasicVector<T>* state_vector) const {
  DRAKE_MBP_THROW_IF_NOT_FINALIZED();
  this->ValidateContext(context);
  state_vector->SetFromVector(GetPositionsAndVelocities(context));
}

template <typename T>
void MultibodyPlant<T>::CopyMultibodyStateOut(
    ModelInstanceIndex model_instance,
    const Context<T>& context, BasicVector<T>* state_vector) const {
  DRAKE_MBP_THROW_IF_NOT_FINALIZED();
  this->ValidateContext(context);
  state_vector->SetFromVector(
      GetPositionsAndVelocities(context, model_instance));
}

template <typename T>
void MultibodyPlant<T>::CopyGeneralizedContactForcesOut(
    const contact_solvers::internal::ContactSolverResults<T>& solver_results,
    ModelInstanceIndex model_instance, BasicVector<T>* tau_vector) const {
  DRAKE_MBP_THROW_IF_NOT_FINALIZED();
  DRAKE_THROW_UNLESS(is_discrete());

  // Vector of generalized contact forces for the entire plant's multibody
  // system.
  const VectorX<T>& tau_contact = solver_results.tau_contact;

  // Generalized velocities and generalized forces are ordered in the same way.
  // Thus we can call get_velocities_from_array().
  const VectorX<T> instance_tau_contact =
      GetVelocitiesFromArray(model_instance, tau_contact);

  tau_vector->set_value(instance_tau_contact);
}

template <typename T>
const systems::InputPort<T>&
MultibodyPlant<T>::get_applied_generalized_force_input_port() const {
  DRAKE_MBP_THROW_IF_NOT_FINALIZED();
  return this->get_input_port(applied_generalized_force_input_port_);
}

template <typename T>
const systems::InputPort<T>&
MultibodyPlant<T>::get_actuation_input_port(
    ModelInstanceIndex model_instance) const {
  DRAKE_MBP_THROW_IF_NOT_FINALIZED();
  DRAKE_THROW_UNLESS(model_instance.is_valid());
  DRAKE_THROW_UNLESS(model_instance < num_model_instances());
  return systems::System<T>::get_input_port(
      instance_actuation_ports_.at(model_instance));
}

template <typename T>
const systems::InputPort<T>& MultibodyPlant<T>::get_actuation_input_port()
    const {
  DRAKE_MBP_THROW_IF_NOT_FINALIZED();
  return systems::System<T>::get_input_port(actuation_port_);
}

template <typename T>
const systems::InputPort<T>&
MultibodyPlant<T>::get_applied_spatial_force_input_port() const {
  DRAKE_MBP_THROW_IF_NOT_FINALIZED();
  return systems::System<T>::get_input_port(applied_spatial_force_input_port_);
}

template <typename T>
const systems::OutputPort<T>& MultibodyPlant<T>::get_state_output_port() const {
  DRAKE_MBP_THROW_IF_NOT_FINALIZED();
  return this->get_output_port(state_output_port_);
}

template <typename T>
const systems::OutputPort<T>& MultibodyPlant<T>::get_state_output_port(
    ModelInstanceIndex model_instance) const {
  DRAKE_MBP_THROW_IF_NOT_FINALIZED();
  DRAKE_THROW_UNLESS(model_instance.is_valid());
  DRAKE_THROW_UNLESS(model_instance < num_model_instances());
  return this->get_output_port(
      instance_state_output_ports_.at(model_instance));
}

template <typename T>
const systems::OutputPort<T>&
MultibodyPlant<T>::get_generalized_acceleration_output_port() const {
  DRAKE_MBP_THROW_IF_NOT_FINALIZED();
  return this->get_output_port(generalized_acceleration_output_port_);
}

template <typename T>
const systems::OutputPort<T>&
MultibodyPlant<T>::get_generalized_acceleration_output_port(
    ModelInstanceIndex model_instance) const {
  DRAKE_MBP_THROW_IF_NOT_FINALIZED();
  DRAKE_THROW_UNLESS(model_instance.is_valid());
  DRAKE_THROW_UNLESS(model_instance < num_model_instances());
  return this->get_output_port(
      instance_generalized_acceleration_output_ports_.at(model_instance));
}

template <typename T>
const systems::OutputPort<T>&
MultibodyPlant<T>::get_generalized_contact_forces_output_port(
    ModelInstanceIndex model_instance) const {
  DRAKE_MBP_THROW_IF_NOT_FINALIZED();
  DRAKE_THROW_UNLESS(model_instance.is_valid());
  DRAKE_THROW_UNLESS(model_instance < num_model_instances());
  return this->get_output_port(
      instance_generalized_contact_forces_output_ports_.at(model_instance));
}

template <typename T>
const systems::OutputPort<T>&
MultibodyPlant<T>::get_contact_results_output_port() const {
  DRAKE_MBP_THROW_IF_NOT_FINALIZED();
  return this->get_output_port(contact_results_port_);
}

template <typename T>
const systems::OutputPort<T>&
MultibodyPlant<T>::get_reaction_forces_output_port() const {
  DRAKE_MBP_THROW_IF_NOT_FINALIZED();
  return this->get_output_port(reaction_forces_port_);
}

template <typename T>
void MultibodyPlant<T>::DeclareSceneGraphPorts() {
  geometry_query_port_ = this->DeclareAbstractInputPort(
      "geometry_query", Value<geometry::QueryObject<T>>{}).get_index();
  geometry_pose_port_ = this->DeclareAbstractOutputPort(
      "geometry_pose", &MultibodyPlant<T>::CalcFramePoseOutput,
      {this->configuration_ticket()}).get_index();
}

template <typename T>
void MultibodyPlant<T>::CalcBodyPosesOutput(
    const Context<T>& context,
    std::vector<math::RigidTransform<T>>* X_WB_all) const {
  DRAKE_MBP_THROW_IF_NOT_FINALIZED();
  this->ValidateContext(context);
  X_WB_all->resize(num_bodies());
  for (BodyIndex body_index(0); body_index < this->num_bodies(); ++body_index) {
    const Body<T>& body = get_body(body_index);
    X_WB_all->at(body_index) = EvalBodyPoseInWorld(context, body);
  }
}

template <typename T>
void MultibodyPlant<T>::CalcBodySpatialVelocitiesOutput(
    const Context<T>& context,
    std::vector<SpatialVelocity<T>>* V_WB_all) const {
  DRAKE_MBP_THROW_IF_NOT_FINALIZED();
  this->ValidateContext(context);
  V_WB_all->resize(num_bodies());
  for (BodyIndex body_index(0); body_index < this->num_bodies(); ++body_index) {
    const Body<T>& body = get_body(body_index);
    V_WB_all->at(body_index) = EvalBodySpatialVelocityInWorld(context, body);
  }
}

template <typename T>
void MultibodyPlant<T>::CalcBodySpatialAccelerationsOutput(
    const Context<T>& context,
    std::vector<SpatialAcceleration<T>>* A_WB_all) const {
  DRAKE_MBP_THROW_IF_NOT_FINALIZED();
  this->ValidateContext(context);
  A_WB_all->resize(num_bodies());
  const AccelerationKinematicsCache<T>& ac = this->EvalForwardDynamics(context);
  for (BodyIndex body_index(0); body_index < this->num_bodies(); ++body_index) {
    const Body<T>& body = get_body(body_index);
    A_WB_all->at(body_index) = ac.get_A_WB(body.node_index());
  }
}

template <typename T>
const SpatialAcceleration<T>&
MultibodyPlant<T>::EvalBodySpatialAccelerationInWorld(
    const Context<T>& context,
    const Body<T>& body_B) const {
  DRAKE_MBP_THROW_IF_NOT_FINALIZED();
  this->ValidateContext(context);
  DRAKE_DEMAND(this == &body_B.GetParentPlant());
  this->ValidateContext(context);
  const AccelerationKinematicsCache<T>& ac = this->EvalForwardDynamics(context);
  return ac.get_A_WB(body_B.node_index());
}

template <typename T>
void MultibodyPlant<T>::CalcFramePoseOutput(
    const Context<T>& context, FramePoseVector<T>* poses) const {
  DRAKE_MBP_THROW_IF_NOT_FINALIZED();
  this->ValidateContext(context);
  const internal::PositionKinematicsCache<T>& pc =
      EvalPositionKinematics(context);

  // NOTE: The body index to frame id map *always* includes the world body but
  // the world body does *not* get reported in the frame poses; only dynamic
  // frames do.
  // TODO(amcastro-tri): Make use of Body::EvalPoseInWorld(context) once caching
  // lands.
  poses->clear();
  for (const auto& it : body_index_to_frame_id_) {
    const BodyIndex body_index = it.first;
    if (body_index == world_index()) continue;
    const Body<T>& body = get_body(body_index);

    // NOTE: The GeometryFrames for each body were registered in the world
    // frame, so we report poses in the world frame.
    poses->set_value(body_index_to_frame_id_.at(body_index),
                     pc.get_X_WB(body.node_index()));
  }
}

template <typename T>
void MultibodyPlant<T>::CalcReactionForces(
    const systems::Context<T>& context,
    std::vector<SpatialForce<T>>* F_CJc_Jc_array) const {
  this->ValidateContext(context);
  DRAKE_DEMAND(F_CJc_Jc_array != nullptr);
  DRAKE_DEMAND(ssize(*F_CJc_Jc_array) == num_joints());

  // Guard against failure to acquire the geometry input deep in the call graph.
  ValidateGeometryInput(context, get_reaction_forces_output_port());

  const VectorX<T>& vdot = this->EvalForwardDynamics(context).get_vdot();

  // TODO(sherm1) EvalForwardDynamics() should record the forces it used
  //              so that we don't have to attempt to reconstruct them
  //              here (and this is broken, see #13888).
  MultibodyForces<T> applied_forces(*this);
  CalcNonContactForces(context, is_discrete(), &applied_forces);
  auto& Fapplied_Bo_W_array = applied_forces.mutable_body_forces();
  auto& tau_applied = applied_forces.mutable_generalized_forces();

  // Add in forces due to contact.
  // Only add in hydroelastic contact forces for continuous mode for now as
  // the forces computed by CalcHydroelasticContactForces() are wrong in
  // discrete mode. See (#13888).
  if (!is_discrete()) {
    CalcAndAddSpatialContactForcesContinuous(context, &Fapplied_Bo_W_array);
  } else {
    CalcAndAddContactForcesByPenaltyMethod(context, &Fapplied_Bo_W_array);
  }

  // Compute reaction forces at each mobilizer.
  std::vector<SpatialAcceleration<T>> A_WB_vector(num_bodies());
  std::vector<SpatialForce<T>> F_BMo_W_vector(num_bodies());
  VectorX<T> tau_id(num_velocities());
  internal_tree().CalcInverseDynamics(context, vdot, Fapplied_Bo_W_array,
                                      tau_applied, &A_WB_vector,
                                      &F_BMo_W_vector, &tau_id);
  // Since vdot is the result of Fapplied and tau_applied we expect the result
  // from inverse dynamics to be zero.
  // TODO(amcastro-tri): find a better estimation for this bound. For instance,
  // we can make an estimation based on the trace of the mass matrix (Jain 2011,
  // Eq. 4.21). For now we only ASSERT though with a better estimation we could
  // promote this to a DEMAND.
  // TODO(amcastro-tri) Uncomment this line once issue #12473 is resolved.
  // DRAKE_ASSERT(tau_id.norm() <
  //              100 * num_velocities() *
  //              std::numeric_limits<double>::epsilon());

  // Map mobilizer reaction forces to joint reaction forces and perform the
  // necessary frame conversions.
  for (JointIndex joint_index(0); joint_index < num_joints(); ++joint_index) {
    const Joint<T>& joint = get_joint(joint_index);
    const internal::MobilizerIndex mobilizer_index =
        internal_tree().get_joint_mobilizer(joint_index);
    const internal::Mobilizer<T>& mobilizer =
        internal_tree().get_mobilizer(mobilizer_index);
    const internal::BodyNodeIndex body_node_index =
        mobilizer.get_topology().body_node;

    // Force on mobilized body B at mobilized frame's origin Mo, expressed in
    // world frame.
    const SpatialForce<T>& F_BMo_W = F_BMo_W_vector[body_node_index];

    // Frames:
    const Frame<T>& frame_Jp = joint.frame_on_parent();
    const Frame<T>& frame_Jc = joint.frame_on_child();
    const FrameIndex F_index = mobilizer.inboard_frame().index();
    const FrameIndex M_index = mobilizer.outboard_frame().index();
    const FrameIndex Jp_index = frame_Jp.index();
    const FrameIndex Jc_index = frame_Jc.index();

    // In Drake we have either:
    //  - Jp == F and Jc == M (typical case)
    //  - Jp == M and Jc == F (mobilizer was inverted)
    // We verify this:
    DRAKE_DEMAND((Jp_index == F_index && Jc_index == M_index) ||
                 (Jp_index == M_index && Jc_index == F_index));

    SpatialForce<T> F_CJc_W;
    if (Jc_index == M_index) {
      // Given we now Mo == Jc and B == C.
      F_CJc_W = F_BMo_W;
    } else if (joint.frame_on_child().index() ==
               mobilizer.inboard_frame().index()) {
      // Given we now Mo == Jc and B == C.
      const SpatialForce<T>& F_PJp_W = F_BMo_W;

      // Newton's third law allows to find the reaction on the child body as
      // required.
      const SpatialForce<T> F_CJp_W = -F_PJp_W;

      // Now we need to shift the application point from Jp to Jc.
      // First we need to find the position vector p_JpJc_W.
      const RotationMatrix<T> R_WJp =
          frame_Jp.CalcRotationMatrixInWorld(context);
      const RigidTransform<T> X_JpJc = frame_Jc.CalcPose(context, frame_Jp);
      const Vector3<T> p_JpJc_Jp = X_JpJc.translation();
      const Vector3<T> p_JpJc_W = R_WJp * p_JpJc_Jp;

      // Finally, we shift the spatial force at Jp.
      F_CJc_W = F_CJp_W.Shift(p_JpJc_W);
    }

    // Re-express in the joint's child frame Jc.
    const RotationMatrix<T> R_WJc = frame_Jc.CalcRotationMatrixInWorld(context);
    const RotationMatrix<T> R_JcW = R_WJc.inverse();
    F_CJc_Jc_array->at(joint_index) = R_JcW * F_CJc_W;
  }
}

template <typename T>
const OutputPort<T>& MultibodyPlant<T>::get_body_poses_output_port()
const {
  DRAKE_MBP_THROW_IF_NOT_FINALIZED();
  return systems::System<T>::get_output_port(body_poses_port_);
}

template <typename T>
const OutputPort<T>&
MultibodyPlant<T>::get_body_spatial_velocities_output_port() const {
  DRAKE_MBP_THROW_IF_NOT_FINALIZED();
  return systems::System<T>::get_output_port(body_spatial_velocities_port_);
}

template <typename T>
const OutputPort<T>&
MultibodyPlant<T>::get_body_spatial_accelerations_output_port() const {
  DRAKE_MBP_THROW_IF_NOT_FINALIZED();
  return systems::System<T>::get_output_port(body_spatial_accelerations_port_);
}

template <typename T>
const OutputPort<T>& MultibodyPlant<T>::get_geometry_poses_output_port()
const {
  return systems::System<T>::get_output_port(geometry_pose_port_);
}

template <typename T>
const systems::InputPort<T>&
MultibodyPlant<T>::get_geometry_query_input_port() const {
  return systems::System<T>::get_input_port(geometry_query_port_);
}

template <typename T>
void MultibodyPlant<T>::ThrowIfFinalized(const char* source_method) const {
  if (is_finalized()) {
    throw std::logic_error(
        "Post-finalize calls to '" + std::string(source_method) + "()' are "
        "not allowed; calls to this method must happen before Finalize().");
  }
}

template <typename T>
void MultibodyPlant<T>::ThrowIfNotFinalized(const char* source_method) const {
  if (!is_finalized()) {
    throw std::logic_error(
        "Pre-finalize calls to '" + std::string(source_method) + "()' are "
        "not allowed; you must call Finalize() first.");
  }
}

template <typename T>
void MultibodyPlant<T>::RemoveUnsupportedScalars(
    const internal::ScalarConvertibleComponent<T>& component) {
  systems::SystemScalarConverter& scalar_converter =
      this->get_mutable_system_scalar_converter();
  if (!component.is_cloneable_to_double()) {
    scalar_converter.Remove<double, T>();
  }
  if (!component.is_cloneable_to_autodiff()) {
    scalar_converter.Remove<AutoDiffXd, T>();
  }
  if (!component.is_cloneable_to_symbolic()) {
    scalar_converter.Remove<symbolic::Expression, T>();
  }
}

template <typename T>
std::vector<std::set<BodyIndex>>
MultibodyPlant<T>::FindSubgraphsOfWeldedBodies() const {
  return multibody_graph_.FindSubgraphsOfWeldedBodies();
}

template <typename T>
T MultibodyPlant<T>::StribeckModel::ComputeFrictionCoefficient(
    const T& speed_BcAc,
    const CoulombFriction<double>& friction) const {
  DRAKE_ASSERT(speed_BcAc >= 0);
  const double mu_d = friction.dynamic_friction();
  const double mu_s = friction.static_friction();
  const T v = speed_BcAc * inv_v_stiction_tolerance_;
  if (v >= 3) {
    return mu_d;
  } else if (v >= 1) {
    return mu_s - (mu_s - mu_d) * step5((v - 1) / 2);
  } else {
    return mu_s * step5(v);
  }
}

template <typename T>
T MultibodyPlant<T>::StribeckModel::step5(const T& x) {
  DRAKE_ASSERT(0 <= x && x <= 1);
  const T x3 = x * x * x;
  return x3 * (10 + x * (6 * x - 15));  // 10x³ - 15x⁴ + 6x⁵
}

template <typename T>
AddMultibodyPlantSceneGraphResult<T>
AddMultibodyPlantSceneGraph(
    systems::DiagramBuilder<T>* builder,
    std::unique_ptr<MultibodyPlant<T>> plant,
    std::unique_ptr<geometry::SceneGraph<T>> scene_graph) {
  DRAKE_DEMAND(builder != nullptr);
  DRAKE_THROW_UNLESS(plant != nullptr);
  plant->set_name("plant");
  if (!scene_graph) {
    scene_graph = std::make_unique<geometry::SceneGraph<T>>();
    scene_graph->set_name("scene_graph");
  }
  auto* plant_ptr = builder->AddSystem(std::move(plant));
  auto* scene_graph_ptr = builder->AddSystem(std::move(scene_graph));
  plant_ptr->RegisterAsSourceForSceneGraph(scene_graph_ptr);
  builder->Connect(
      plant_ptr->get_geometry_poses_output_port(),
      scene_graph_ptr->get_source_pose_port(
          plant_ptr->get_source_id().value()));
  builder->Connect(
      scene_graph_ptr->get_query_output_port(),
      plant_ptr->get_geometry_query_input_port());
  return {plant_ptr, scene_graph_ptr};
}

template <typename T>
AddMultibodyPlantSceneGraphResult<T> AddMultibodyPlantSceneGraph(
    systems::DiagramBuilder<T>* builder, double time_step,
    std::unique_ptr<geometry::SceneGraph<T>> scene_graph) {
  DRAKE_DEMAND(builder != nullptr);
  auto plant = std::make_unique<MultibodyPlant<T>>(time_step);
  plant->set_name("plant");
  return AddMultibodyPlantSceneGraph(builder, std::move(plant),
                                     std::move(scene_graph));
}

DRAKE_DEFINE_FUNCTION_TEMPLATE_INSTANTIATIONS_ON_DEFAULT_SCALARS((
    /* Use static_cast to disambiguate the two different overloads. */
    static_cast<AddMultibodyPlantSceneGraphResult<T>(*)(
        systems::DiagramBuilder<T>*, double,
        std::unique_ptr<geometry::SceneGraph<T>>)>(
            &AddMultibodyPlantSceneGraph),
    /* Use static_cast to disambiguate the two different overloads. */
    static_cast<AddMultibodyPlantSceneGraphResult<T>(*)(
        systems::DiagramBuilder<T>*,
        std::unique_ptr<MultibodyPlant<T>>,
        std::unique_ptr<geometry::SceneGraph<T>>)>(
            &AddMultibodyPlantSceneGraph)
))

}  // namespace multibody
}  // namespace drake

DRAKE_DEFINE_CLASS_TEMPLATE_INSTANTIATIONS_ON_DEFAULT_SCALARS(
    class drake::multibody::MultibodyPlant)
DRAKE_DEFINE_CLASS_TEMPLATE_INSTANTIATIONS_ON_DEFAULT_SCALARS(
    struct drake::multibody::AddMultibodyPlantSceneGraphResult)
