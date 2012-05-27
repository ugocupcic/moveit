/*********************************************************************
* Software License Agreement (BSD License)
*
*  Copyright (c) 2012, Willow Garage, Inc.
*  All rights reserved.
*
*  Redistribution and use in source and binary forms, with or without
*  modification, are permitted provided that the following conditions
*  are met:
*
*   * Redistributions of source code must retain the above copyright
*     notice, this list of conditions and the following disclaimer.
*   * Redistributions in binary form must reproduce the above
*     copyright notice, this list of conditions and the following
*     disclaimer in the documentation and/or other materials provided
*     with the distribution.
*   * Neither the name of the Willow Garage nor the names of its
*     contributors may be used to endorse or promote products derived
*     from this software without specific prior written permission.
*
*  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
*  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
*  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
*  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
*  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
*  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
*  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
*  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
*  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
*  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
*  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
*  POSSIBILITY OF SUCH DAMAGE.
*********************************************************************/

/* Author: Ioan Sucan, Sachin Chitta */

#include "ompl_interface/model_based_planning_context.h"
#include "ompl_interface/detail/state_validity_checker.h"
#include "ompl_interface/detail/constrained_sampler.h"
#include "ompl_interface/detail/constrained_goal_sampler.h"
#include "ompl_interface/detail/goal_union.h"
#include "ompl_interface/detail/projection_evaluators.h"
#include "ompl_interface/constraints_library.h"
#include <kinematic_constraints/utils.h>

#include <ompl/base/samplers/UniformValidStateSampler.h>
#include <ompl/base/GoalLazySamples.h>
#include <ompl/tools/config/SelfConfig.h>
#include <ompl/tools/debug/Profiler.h>
#include <ompl/base/spaces/SE3StateSpace.h>

ompl_interface::ModelBasedPlanningContext::ModelBasedPlanningContext(const std::string &name, const ModelBasedStateSpacePtr &state_space, 
                                                                     const ModelBasedPlanningContextSpecification &spec) :
  spec_(spec), name_(name), ompl_state_space_(state_space), complete_initial_robot_state_(ompl_state_space_->getKinematicModel()),
  ompl_simple_setup_(ompl_state_space_), ompl_benchmark_(ompl_simple_setup_), ompl_parallel_plan_(ompl_simple_setup_.getProblemDefinition()),
  last_plan_time_(0.0), last_simplify_time_(0.0), max_goal_samples_(0), max_state_sampling_attempts_(0), max_goal_sampling_attempts_(0), 
  max_planning_threads_(0), max_velocity_(0), max_acceleration_(0.0), max_solution_segment_length_(0.0)
{
  ompl_simple_setup_.getStateSpace()->computeSignature(space_signature_);
  ompl_simple_setup_.getStateSpace()->setStateSamplerAllocator(boost::bind(&ModelBasedPlanningContext::allocPathConstrainedSampler, this, _1));
}

void ompl_interface::ModelBasedPlanningContext::setProjectionEvaluator(const std::string &peval)
{
  if (!ompl_state_space_)
  {
    ROS_ERROR("No state space is configured yet");
    return;
  }
  ob::ProjectionEvaluatorPtr pe = getProjectionEvaluator(peval);
  if (pe)
    ompl_state_space_->registerDefaultProjection(pe);
}

ompl::base::ProjectionEvaluatorPtr ompl_interface::ModelBasedPlanningContext::getProjectionEvaluator(const std::string &peval) const
{
  if (peval.find_first_of("link(") == 0 && peval[peval.length() - 1] == ')')
  {
    std::string link_name = peval.substr(5, peval.length() - 6);
    if (getKinematicModel()->hasLinkModel(link_name))
      return ob::ProjectionEvaluatorPtr(new ProjectionEvaluatorLinkPose(this, link_name));
    else
      ROS_ERROR("Attempted to set projection evaluator with respect to position of link '%s', but that link is not known to the kinematic model.", link_name.c_str());
  }
  else
    if (peval.find_first_of("joints(") == 0 && peval[peval.length() - 1] == ')')
    {
      std::string joints = peval.substr(7, peval.length() - 8);
      boost::replace_all(joints, ",", " ");
      std::vector<std::pair<std::string, unsigned int> > j;
      std::stringstream ss(joints);
      while (ss.good() && !ss.eof())
      {
	std::string v; ss >> v >> std::ws;
	if (getKinematicModel()->hasJointModel(v))
	{
	  unsigned int vc = getKinematicModel()->getJointModel(v)->getVariableCount();
	  if (vc > 0)
	    j.push_back(std::make_pair(v, vc));
	  else
	    ROS_WARN("%s: Ignoring joint '%s' in projection since it has 0 DOF", name_.c_str(), v.c_str());
	}
	else
	  ROS_ERROR("%s: Attempted to set projection evaluator with respect to value of joint '%s', but that joint is not known to the kinematic model.",
		    name_.c_str(), v.c_str());
      }
      if (j.empty())
	ROS_ERROR("%s: No valid joints specified for joint projection", name_.c_str());
      else
	return ob::ProjectionEvaluatorPtr(new ProjectionEvaluatorJointValue(this, j));
    }
    else
      ROS_ERROR("Unable to allocate projection evaluator based on description: '%s'", peval.c_str());  
  return ob::ProjectionEvaluatorPtr();
}

ompl::base::StateSamplerPtr ompl_interface::ModelBasedPlanningContext::allocPathConstrainedSampler(const ompl::base::StateSpace *ss) const
{
  if (ompl_state_space_.get() != ss)
    ROS_FATAL("%s: Attempted to allocate a state sampler for an unknown state space", name_.c_str());
  ROS_DEBUG("%s: Allocating a new state sampler (attempts to use path constraints)", name_.c_str());
  
  if (path_constraints_)
  {
    if (spec_.constraints_library_)
    {
      const ConstraintApproximationPtr &ca = spec_.constraints_library_->getConstraintApproximation(path_constraints_msg_);
      if (ca)
      {
        ompl::base::StateSamplerAllocator c_ssa = ca->getStateSamplerAllocator(path_constraints_msg_);
        if (c_ssa)
        {
          ompl::base::StateSamplerPtr res = c_ssa(ss);
          if (res)
          {
            ROS_DEBUG("Using precomputed state sampler (approximated constraint space)");
            return res;
          }
        }
      }
    }  

    constraint_samplers::ConstraintSamplerPtr cs;
    if (spec_.constraint_sampler_manager_)
      cs = spec_.constraint_sampler_manager_->selectSampler(getPlanningScene(), getJointModelGroup()->getName(), path_constraints_->getAllConstraints());
    
    if (cs)
    {
      ROS_DEBUG("%s: Allocating specialized state sampler for state space", name_.c_str());
      return ob::StateSamplerPtr(new ConstrainedSampler(this, cs));
    }
  }
  ROS_DEBUG("%s: Allocating default state sampler for state space", name_.c_str());
  return ss->allocDefaultStateSampler();
}

void ompl_interface::ModelBasedPlanningContext::configure(void)
{
  // convert the input state to the corresponding OMPL state
  ompl::base::ScopedState<> ompl_start_state(ompl_state_space_);
  ompl_state_space_->copyToOMPLState(ompl_start_state.get(), getCompleteInitialRobotState());
  ompl_start_state->as<ModelBasedStateSpace::StateType>()->markStartState();
  ompl_simple_setup_.setStartState(ompl_start_state);
  ompl_simple_setup_.setStateValidityChecker(ob::StateValidityCheckerPtr(new StateValidityChecker(this)));
    
  useConfig();  
  if (ompl_simple_setup_.getGoal())
    ompl_simple_setup_.setup();
}

void ompl_interface::ModelBasedPlanningContext::useConfig(void)
{
  const std::map<std::string, std::string> &config = spec_.config_;
  if (config.empty())
    return;
  std::map<std::string, std::string> cfg = config;
  
  // set the projection evaluator
  std::map<std::string, std::string>::iterator it = cfg.find("projection_evaluator");
  if (it != cfg.end())
  {
    setProjectionEvaluator(boost::trim_copy(it->second));
    cfg.erase(it);
  }
  
  it = cfg.find("max_velocity");
  if (it != cfg.end())
  {
    try
    {
      max_velocity_ = boost::lexical_cast<double>(boost::trim_copy(it->second));
      ROS_INFO("%s: Maximum velocity set to %lf", name_.c_str(), max_velocity_);
    }
    catch(boost::bad_lexical_cast &e)
    {
      ROS_ERROR("%s: Unable to parse maximum velocity: %s", name_.c_str(), e.what());
    }
    cfg.erase(it);
  }
  
  it = cfg.find("max_acceleration");
  if (it != cfg.end())
  {
    try
    {
      max_velocity_ = boost::lexical_cast<double>(boost::trim_copy(it->second));
      ROS_INFO("%s: Maximum acceleration set to %lf", name_.c_str(), max_velocity_);
    }
    catch(boost::bad_lexical_cast &e)
    {
      ROS_ERROR("%s: Unable to parse maximum acceleration: %s", name_.c_str(), e.what());
    }
    cfg.erase(it);
  }
  
  if (cfg.empty())
    return;
  
  it = cfg.find("type");
  if (it == cfg.end())
  {
    if (name_ != getJointModelGroupName())
      ROS_WARN("%s: Attribute 'type' not specified in planner configuration", name_.c_str());
  }
  else
  {
    // remove the 'type' parameter; the rest are parameters for the planner itself
    std::string type = it->second;
    cfg.erase(it);
    ompl_simple_setup_.setPlannerAllocator(boost::bind(spec_.planner_allocator_, _1, type, 
						       name_ != getJointModelGroupName() ? name_ : "", cfg));
    ROS_INFO("Planner configuration '%s' will use planner '%s'. Additional configuration parameters will be set when the planner is constructed.",
	     name_.c_str(), type.c_str());
  }
  
  // call the setParams() after setup(), so we know what the params are
  ompl_simple_setup_.getSpaceInformation()->setup();
  ompl_simple_setup_.getSpaceInformation()->params().setParams(cfg, true);
  // call setup() again for possibly new param values
  ompl_simple_setup_.getSpaceInformation()->setup();
}

void ompl_interface::ModelBasedPlanningContext::setPlanningVolume(const moveit_msgs::WorkspaceParameters &wparams)
{
  if (wparams.min_corner.x == wparams.max_corner.x && wparams.min_corner.x == 0.0 &&
      wparams.min_corner.y == wparams.max_corner.y && wparams.min_corner.y == 0.0 &&
      wparams.min_corner.z == wparams.max_corner.z && wparams.min_corner.z == 0.0)
  {
    ROS_DEBUG("It looks like the planning volume was not specified. Using default values.");
    moveit_msgs::WorkspaceParameters default_wp;
    default_wp.min_corner.x = default_wp.min_corner.y = default_wp.min_corner.z = -1.0;
    default_wp.max_corner.x = default_wp.max_corner.y = default_wp.max_corner.z = 1.0;
    setPlanningVolume(default_wp);    
    return;
  }
  
  ROS_DEBUG("%s: Setting planning volume (affects SE2 & SE3 joints only) to x = [%f, %f], y = [%f, %f], z = [%f, %f]", name_.c_str(),
	    wparams.min_corner.x, wparams.max_corner.x, wparams.min_corner.y, wparams.max_corner.y, wparams.min_corner.z, wparams.max_corner.z);
  
  ompl_state_space_->setBounds(wparams.min_corner.x, wparams.max_corner.x,
                               wparams.min_corner.y, wparams.max_corner.y,
                               wparams.min_corner.z, wparams.max_corner.z);
}

void ompl_interface::ModelBasedPlanningContext::simplifySolution(double timeout)
{
  ompl_simple_setup_.simplifySolution(timeout);
  last_simplify_time_ = ompl_simple_setup_.getLastSimplificationTime();
}

void ompl_interface::ModelBasedPlanningContext::interpolateSolution(void)
{
  if (ompl_simple_setup_.haveSolutionPath())
  {
    og::PathGeometric &pg = ompl_simple_setup_.getSolutionPath();
    pg.interpolate((std::size_t)floor(0.5 + pg.length() / max_solution_segment_length_));
  }
}

void ompl_interface::ModelBasedPlanningContext::convertPath(const ompl::geometric::PathGeometric &pg, moveit_msgs::RobotTrajectory &traj) const
{
  planning_models::KinematicState ks = complete_initial_robot_state_;
  const std::vector<const planning_models::KinematicModel::JointModel*> &jnt = getJointModelGroup()->getJointModels();
  std::vector<const planning_models::KinematicModel::JointModel*> onedof;
  std::vector<const planning_models::KinematicModel::JointModel*> mdof;
  traj.joint_trajectory.header.frame_id = getPlanningScene()->getPlanningFrame();
  traj.joint_trajectory.joint_names.clear();
  traj.multi_dof_joint_trajectory.joint_names.clear();
  traj.multi_dof_joint_trajectory.child_frame_ids.clear();
  for (std::size_t i = 0 ; i < jnt.size() ; ++i)
    if (jnt[i]->getVariableCount() == 1)
    {
      traj.joint_trajectory.joint_names.push_back(jnt[i]->getName());
      onedof.push_back(jnt[i]);
    }
    else
    {
      traj.multi_dof_joint_trajectory.joint_names.push_back(jnt[i]->getName());
      traj.multi_dof_joint_trajectory.frame_ids.push_back(traj.joint_trajectory.header.frame_id);
      traj.multi_dof_joint_trajectory.child_frame_ids.push_back(jnt[i]->getChildLinkModel()->getName());
      mdof.push_back(jnt[i]);
    }
  if (!onedof.empty())
    traj.joint_trajectory.points.resize(pg.getStateCount());
  if (!mdof.empty())
    traj.multi_dof_joint_trajectory.points.resize(pg.getStateCount());
  std::vector<double> times;
  pg.computeFastTimeParametrization(max_velocity_, max_acceleration_, times, 50);
  for (std::size_t i = 0 ; i < pg.getStateCount() ; ++i)
  {
    ompl_state_space_->copyToKinematicState(ks, pg.getState(i));
    if (!onedof.empty())
    {
      traj.joint_trajectory.points[i].positions.resize(onedof.size());
      for (std::size_t j = 0 ; j < onedof.size() ; ++j)
	traj.joint_trajectory.points[i].positions[j] = ks.getJointState(onedof[j]->getName())->getVariableValues()[0];
      traj.joint_trajectory.points[i].time_from_start = ros::Duration(times[i]);
    }
    if (!mdof.empty())
    {
      traj.multi_dof_joint_trajectory.points[i].poses.resize(mdof.size());
      for (std::size_t j = 0 ; j < mdof.size() ; ++j)
      {
	planning_models::msgFromPose(ks.getJointState(mdof[j]->getName())->getVariableTransform(),
				     traj.multi_dof_joint_trajectory.points[i].poses[j]);
      }
      traj.multi_dof_joint_trajectory.points[i].time_from_start = ros::Duration(times[i]);
    }
  }
}

bool ompl_interface::ModelBasedPlanningContext::getSolutionPath(moveit_msgs::RobotTrajectory &traj) const
{
  if (!ompl_simple_setup_.haveSolutionPath())
    return false;
  convertPath(ompl_simple_setup_.getSolutionPath(), traj);
  return true;
}

void ompl_interface::ModelBasedPlanningContext::setVerboseStateValidityChecks(bool flag)
{
  if (ompl_simple_setup_.getStateValidityChecker())
    static_cast<StateValidityChecker*>(ompl_simple_setup_.getStateValidityChecker().get())->setVerbose(flag);
}

ompl::base::GoalPtr ompl_interface::ModelBasedPlanningContext::constructGoal(void)
{ 
  // ******************* set up the goal representation, based on goal constraints
  
  std::vector<ob::GoalPtr> goals;
  for (std::size_t i = 0 ; i < goal_constraints_.size() ; ++i)
  {
    constraint_samplers::ConstraintSamplerPtr cs;
    if (spec_.constraint_sampler_manager_)
      cs = spec_.constraint_sampler_manager_->selectSampler(getPlanningScene(), getJointModelGroup()->getName(), goal_constraints_[i]->getAllConstraints());
    ob::GoalPtr g = ob::GoalPtr(new ConstrainedGoalSampler(this, goal_constraints_[i], cs));
    goals.push_back(g);
  }
  
  if (!goals.empty())
    return goals.size() == 1 ? goals[0] : ompl::base::GoalPtr(new GoalSampleableRegionMux(goals));
  else
    ROS_ERROR("Unable to construct goal representation");
  
  return ob::GoalPtr();
}

void ompl_interface::ModelBasedPlanningContext::setPlanningScene(const planning_scene::PlanningSceneConstPtr &planning_scene)
{
  planning_scene_ = planning_scene;
}

void ompl_interface::ModelBasedPlanningContext::setStartState(const planning_models::KinematicState &complete_initial_robot_state)
{
  complete_initial_robot_state_ = complete_initial_robot_state;
}

void ompl_interface::ModelBasedPlanningContext::clear(void)
{
  ompl_simple_setup_.clear();
  ompl_simple_setup_.clearStartStates();
  ompl_simple_setup_.setGoal(ob::GoalPtr());  
  ompl_simple_setup_.setStateValidityChecker(ob::StateValidityCheckerPtr());
  path_constraints_.reset();
  goal_constraints_.clear();
}

bool ompl_interface::ModelBasedPlanningContext::setRandomStartGoal(void)
{
  ob::ValidStateSamplerPtr vss(new ob::UniformValidStateSampler(ompl_simple_setup_.getSpaceInformation().get()));
  vss->setNrAttempts(10000);
  ob::ScopedState<> ss(ompl_state_space_);
  if (vss->sample(ss.get()))
  {
    ompl_state_space_->copyToKinematicState(complete_initial_robot_state_, ss.get());
    ROS_INFO("Selected a random valid start state");
    if (vss->sample(ss.get()))
    {
      ompl_simple_setup_.setGoalState(ss);
      ROS_INFO("Selected a random valid goal state");
      return true;
    }
    else
      ROS_WARN("Unable to select random valid goals state");
  }
  else
    ROS_WARN("Unable to select random valid start/goal states");
  return false;
}

bool ompl_interface::ModelBasedPlanningContext::setPathConstraints(const moveit_msgs::Constraints &path_constraints,
								   moveit_msgs::MoveItErrorCodes *error)
{
  // ******************* set the path constraints to use
  path_constraints_.reset(new kinematic_constraints::KinematicConstraintSet(getPlanningScene()->getKinematicModel(), getPlanningScene()->getTransforms()));
  path_constraints_->add(path_constraints);
  path_constraints_msg_ = path_constraints;
  
  return true;
}
								   
bool ompl_interface::ModelBasedPlanningContext::setGoalConstraints(const std::vector<moveit_msgs::Constraints> &goal_constraints,
								   const moveit_msgs::Constraints &path_constraints,
								   moveit_msgs::MoveItErrorCodes *error)
{
  
  // ******************* check if the input is correct
  goal_constraints_.clear();
  for (std::size_t i = 0 ; i < goal_constraints.size() ; ++i)
  {
    moveit_msgs::Constraints constr = kinematic_constraints::mergeConstraints(goal_constraints[i], path_constraints);
    kinematic_constraints::KinematicConstraintSetPtr kset(new kinematic_constraints::KinematicConstraintSet(getPlanningScene()->getKinematicModel(), getPlanningScene()->getTransforms()));
    kset->add(constr);
    if (!kset->empty())
      goal_constraints_.push_back(kset);
  }
  
  if (goal_constraints_.empty())
  {
    ROS_WARN("%s: No goal constraints specified. There is no problem to solve.", name_.c_str());
    if (error)
      error->val = moveit_msgs::MoveItErrorCodes::INVALID_GOAL_CONSTRAINTS;
    return false;
  }
  
  ob::GoalPtr goal = constructGoal();
  ompl_simple_setup_.setGoal(goal);
  if (goal)
    return true;
  else
    return false;
}

bool ompl_interface::ModelBasedPlanningContext::benchmark(double timeout, unsigned int count, const std::string &filename)
{
  ompl_benchmark_.clearPlanners();
  ompl_simple_setup_.setup();  
  ompl_benchmark_.addPlanner(ompl_simple_setup_.getPlanner());
  ompl_benchmark_.setExperimentName(getKinematicModel()->getName() + "_" + getJointModelGroupName() + "_" +
				    getPlanningScene()->getName() + "_" + name_);
  
  ot::Benchmark::Request req;
  req.maxTime = timeout;
  req.runCount = count;
  req.displayProgress = true;
  req.saveConsoleOutput = false;
  ompl_benchmark_.benchmark(req);
  return filename.empty() ? ompl_benchmark_.saveResultsToFile() : ompl_benchmark_.saveResultsToFile(filename.c_str());
}

bool ompl_interface::ModelBasedPlanningContext::fixInvalidInputStates(const ompl::time::point &end_time)
{
  // try to fix invalid input states, if any
  static const double INITIAL_DISTANCE_DIVISOR = 1000.0;
  static const double DISTANCE_INCREASE_FACTOR = 5.0;
  static const unsigned int MAX_INCREASE_STEPS = (unsigned int)(log(INITIAL_DISTANCE_DIVISOR) / log(DISTANCE_INCREASE_FACTOR));
  static const unsigned int FIX_ATTEMPTS = 100;
  
  double d = ompl_simple_setup_.getStateSpace()->getMaximumExtent() / INITIAL_DISTANCE_DIVISOR;
  bool fixed = false;
  unsigned int steps = 0;
  do
  {
    steps++;
    if (ompl_simple_setup_.getProblemDefinition()->fixInvalidInputStates(d, d, FIX_ATTEMPTS))
      fixed = true;
    else
      d *= DISTANCE_INCREASE_FACTOR;
  } while (!fixed && steps < MAX_INCREASE_STEPS && ompl::time::now() < end_time);
  
  return fixed;
}

bool ompl_interface::ModelBasedPlanningContext::solve(double timeout, unsigned int count)
{
  ot::Profiler::ScopedBlock sblock("PlanningContextSolve");

  ompl::time::point start = ompl::time::now();
  
  // clear previously computed solutions
  ompl_simple_setup_.getProblemDefinition()->clearSolutionPaths();
  const ob::PlannerPtr planner = ompl_simple_setup_.getPlanner();
  if (planner)
    planner->clear();
  bool gls = ompl_simple_setup_.getGoal()->hasType(ob::GOAL_LAZY_SAMPLES);
  // just in case sampling is not started
  if (gls)
    static_cast<ob::GoalLazySamples*>(ompl_simple_setup_.getGoal().get())->startSampling();
  
  // don't fix invalid states on purpose; this should be handled by planning request adapters
  //  fixInvalidInputStates(end_time);
  
  ompl_simple_setup_.getSpaceInformation()->getMotionValidator()->resetMotionCounter();
  
  bool result = false;
  if (count <= 1)
  {
    ROS_DEBUG("%s: Solving the planning problem once...", name_.c_str());
    ob::PlannerTerminationCondition ptc = ob::timedPlannerTerminationCondition(timeout - ompl::time::seconds(ompl::time::now() - start));
    registerTerminationCondition(ptc);
    result = ompl_simple_setup_.solve(ptc);
    last_plan_time_ = ompl_simple_setup_.getLastPlanComputationTime();
    unregisterTerminationCondition();
  }
  else
  {
    ROS_DEBUG("%s: Solving the planning problem %u times...", name_.c_str(), count);
    ompl_parallel_plan_.clearHybridizationPaths();
    if (count <= max_planning_threads_)
    {
      ompl_parallel_plan_.clearPlanners();
      if (ompl_simple_setup_.getPlannerAllocator())
	for (unsigned int i = 0 ; i < count ; ++i)
	  ompl_parallel_plan_.addPlannerAllocator(ompl_simple_setup_.getPlannerAllocator());
      else
	for (unsigned int i = 0 ; i < count ; ++i)
	  ompl_parallel_plan_.addPlanner(ompl::geometric::getDefaultPlanner(ompl_simple_setup_.getGoal())); 

      ob::PlannerTerminationCondition ptc = ob::timedPlannerTerminationCondition(timeout - ompl::time::seconds(ompl::time::now() - start));
      registerTerminationCondition(ptc);
      result = ompl_parallel_plan_.solve(ptc, 1, count, true);
      last_plan_time_ = ompl::time::seconds(ompl::time::now() - start);
      unregisterTerminationCondition();
    }
    else
    {
      ob::PlannerTerminationCondition ptc = ob::timedPlannerTerminationCondition(timeout - ompl::time::seconds(ompl::time::now() - start));
      registerTerminationCondition(ptc);
      int n = count / max_planning_threads_;
      result = true;
      for (int i = 0 ; i < n && ptc() == false ; ++i)
      {
	ompl_parallel_plan_.clearPlanners();
	if (ompl_simple_setup_.getPlannerAllocator())
	  for (unsigned int i = 0 ; i < max_planning_threads_ ; ++i)
	    ompl_parallel_plan_.addPlannerAllocator(ompl_simple_setup_.getPlannerAllocator());
	else
	  for (unsigned int i = 0 ; i < max_planning_threads_ ; ++i)
	    ompl_parallel_plan_.addPlanner(og::getDefaultPlanner(ompl_simple_setup_.getGoal()));
	bool r = ompl_parallel_plan_.solve(ptc, 1, max_planning_threads_, true);
	result = result && r; 
      }
      n = count % max_planning_threads_;
      if (n && ptc() == false)
      {
	ompl_parallel_plan_.clearPlanners();
	if (ompl_simple_setup_.getPlannerAllocator())
	  for (int i = 0 ; i < n ; ++i)
	    ompl_parallel_plan_.addPlannerAllocator(ompl_simple_setup_.getPlannerAllocator());
	else
	  for (int i = 0 ; i < n ; ++i)
	    ompl_parallel_plan_.addPlanner(og::getDefaultPlanner(ompl_simple_setup_.getGoal()));
	bool r = ompl_parallel_plan_.solve(ptc, 1, n, true);
	result = result && r;
      }
      last_plan_time_ = ompl::time::seconds(ompl::time::now() - start);
      unregisterTerminationCondition();
    }
  }
  
  if (gls)
    // just in case we need to stop sampling
    static_cast<ob::GoalLazySamples*>(ompl_simple_setup_.getGoal().get())->stopSampling();
  
  int v = ompl_simple_setup_.getSpaceInformation()->getMotionValidator()->getValidMotionCount();
  int iv = ompl_simple_setup_.getSpaceInformation()->getMotionValidator()->getInvalidMotionCount();
  ROS_DEBUG("There were %d valid motions and %d invalid motions.", v, iv);
  
  if (ompl_simple_setup_.getProblemDefinition()->hasApproximateSolution())
    ROS_WARN("Computed solution is approximate");
  
  return result;
}

void ompl_interface::ModelBasedPlanningContext::registerTerminationCondition(const ob::PlannerTerminationCondition &ptc)
{
  boost::mutex::scoped_lock slock(ptc_lock_);
  ptc_ = &ptc;
}

void ompl_interface::ModelBasedPlanningContext::unregisterTerminationCondition(void)
{ 
  boost::mutex::scoped_lock slock(ptc_lock_);
  ptc_ = NULL;
}

void ompl_interface::ModelBasedPlanningContext::terminateSolve(void)
{
  boost::mutex::scoped_lock slock(ptc_lock_);
  if (ptc_)
    ptc_->terminate();
}
