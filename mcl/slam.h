#include <vector>
#include <set>
#include <functional>

#include <Eigen/Dense>
#include <Eigen/Sparse>

#include "Camera.h"
#include "Landmark.h"
#include "unicycle.h"

namespace mcl {
    namespace slam {

        namespace {
            inline void boxplus_se2(Eigen::Vector3f& x, const Eigen::Vector3f& dx) {
                std::cout << "x before boxplus_se2: " << x.transpose() << std::endl;
                x = mcl::t2v(mcl::v2t(dx) * mcl::v2t(x));
                std::cout << "x after boxplus_se2: " << x.transpose() << std::endl;
            }

            inline void boxplus_euclidean(Eigen::Vector3f& x, const Eigen::Vector3f& dx) {
                x += dx;
            }

            void iter_boxplus(std::vector<Eigen::Vector3f>& x,
                              const std::vector<Eigen::Vector3f>& dx,
                              std::function<void(Eigen::Vector3f&, const Eigen::Vector3f&)> boxplus_op) {
                std::vector<Eigen::Vector3f>::iterator x_it = x.begin();
                std::vector<Eigen::Vector3f>::const_iterator dx_it = dx.cbegin();
                for (; x_it != x.end() && dx_it != dx.cend(); ++x_it, ++dx_it) {
                    boxplus_op(*x_it, *dx_it);
                }
            }
        }

        // estimated_state: extended configuration of the unicycle (x, y, theta, x_l1, y_l1, ...)
        // covariance_estimate: convariance matrix of estimated_state
        // displacement: (x_{t+1}-x_{t}, y_{t+1}-y_{t}, theta_{t+1}-theta{t})
        // control_noise: sigma_u^2
        void predict(Eigen::VectorXf& estimated_state,
                     Eigen::MatrixXf& covariance_estimate,
                     const Eigen::Vector3f& displacement,
                     const float control_noise = 0.001f) {

            Eigen::MatrixXf jacobian_transition = Eigen::MatrixXf::Identity(estimated_state.rows(), estimated_state.rows());
            Eigen::MatrixXf jacobian_controls = Eigen::MatrixXf::Zero(estimated_state.rows(), 3);
            Eigen::Matrix3f covariance_controls;

            jacobian_controls.block<3, 3>(0, 0) = Eigen::Matrix3f::Identity();

            covariance_controls << std::pow(displacement[0], 2.0f) + control_noise, 0.0f, 0.0f,
                                   0.0f, std::pow(displacement[1], 2.0f) + control_noise, 0.0f,
                                   0.0f, 0.0f, std::pow(displacement[2], 2.0f) + control_noise;

            // Perform prediciton step updating estimated_state and covariance_estimate:
            mcl::unicycle::transition(estimated_state, displacement);
            covariance_estimate = jacobian_transition * covariance_estimate * jacobian_transition.transpose() +
                                  jacobian_controls * covariance_controls * jacobian_controls.transpose();
        }

        // estimated_state: configuration of the unicycle (x, y, theta)
        // covariance_estimate: convariance matrix of estimated_state
        // landmarks: vector containing all the information about the landmarks
        // measurements: vector containing all the information about the measurements (current step)
        void update(Eigen::Vector3f& gt_pose,
                    Eigen::VectorXf& estimated_state,
                    Eigen::MatrixXf& covariance_estimate,
                    const std::vector<mcl::Landmark>& landmarks,
                    const std::vector<mcl::Measurement>& measurements,
                    const mcl::Camera& camera,
                    std::map<int, int>& id_to_state_map,
                    std::vector<int>& state_to_id_map) {

            // Estimated transform:
            Eigen::Matrix2f estimated_rotation_robot2world;
            estimated_rotation_robot2world << std::cos(estimated_state[2]), -std::sin(estimated_state[2]),
                                              std::sin(estimated_state[2]),  std::cos(estimated_state[2]);
            Eigen::Matrix2f estimated_rotation_world2robot = estimated_rotation_robot2world.transpose();
            Eigen::Matrix3f estimated_rotation_world2robot_3f = Eigen::Matrix3f::Identity();
            estimated_rotation_world2robot_3f.block<2, 2>(0, 0) = estimated_rotation_world2robot;
            Eigen::Matrix3f estimated_derivative_transpose_rotation_world2robot;
            estimated_derivative_transpose_rotation_world2robot << -std::sin(estimated_state[2]),  std::cos(estimated_state[2]), 0.0f,
                                                                   -std::cos(estimated_state[2]), -std::sin(estimated_state[2]), 0.0f,
                                                                                            0.0f,                          0.0f, 0.0f;

            // GT transform:
            Eigen::Matrix4f gt_transform_robot2world;
            gt_transform_robot2world <<  std::cos(gt_pose[2]), -std::sin(gt_pose[2]), 0.0f, gt_pose[0],
                                         std::sin(gt_pose[2]),  std::cos(gt_pose[2]), 0.0f, gt_pose[1],
                                                         0.0f,                  0.0f, 1.0f,       0.0f,
                                                         0.0f,                  0.0f, 0.0f,       1.0f;
            Eigen::Matrix4f gt_transform_world2camera = (gt_transform_robot2world * camera.transform_rf_parent).inverse();

            const Eigen::Matrix3f rotation_robot2camera = camera.transform_rf_parent.block<3, 3>(0, 0).transpose();

            int tot_measurements_known_landmarks = 0;

            std::vector<Eigen::Vector4f> gt_landmark_position_rf_robot_vector;

            // Quickly count number of known landmarks in current set of
            // measurements to avoid resizing multiple times,
            // NOTE: there could be multiple measurements relative to the
            // same landmark, make sure to count only once:
            std::set<int> id_known_landmarks;
            std::set<int> id_unknown_landmarks;
            for (const auto& meas : measurements) {
                if (id_to_state_map.find(meas.gt_landmark_id) != id_to_state_map.end()) {
                    ++tot_measurements_known_landmarks;
                    id_known_landmarks.insert(meas.gt_landmark_id);
                } else {
                    id_unknown_landmarks.insert(meas.gt_landmark_id);
                }
            }
            const int number_of_unknown_landmarks = id_unknown_landmarks.size();

            Eigen::VectorXf measured_uv(2 * tot_measurements_known_landmarks);
            Eigen::VectorXf estimated_uv(2 * tot_measurements_known_landmarks);
            Eigen::MatrixXf jacobian_measurements = Eigen::MatrixXf::Zero(2 * tot_measurements_known_landmarks, estimated_state.rows());

            int k = 0;
            for (const auto& meas : measurements) {
                // Simulating depth sensor:
                float f  = camera.matrix(0, 0);
                float u0 = camera.matrix(0, 2);
                float v0 = camera.matrix(1, 2);
                float depth = (gt_transform_world2camera *
                    Eigen::Vector4f(landmarks[meas.gt_landmark_id].position.x(),
                                    landmarks[meas.gt_landmark_id].position.y(),
                                    landmarks[meas.gt_landmark_id].position.z(),
                                    1.0f)).z();
                Eigen::Vector4f gt_landmark_position_rf_camera((meas.u - u0) * depth / f, (meas.v - v0) * depth / f, depth, 1.0f);
                Eigen::Vector4f gt_landmark_position_rf_robot = camera.transform_rf_parent * gt_landmark_position_rf_camera;
                gt_landmark_position_rf_robot_vector.push_back(gt_landmark_position_rf_robot);

                // Check if measured landmark has already been seen:
                auto state_map_it = id_to_state_map.find(meas.gt_landmark_id);
                if (state_map_it != id_to_state_map.end()) {
                    // Compute bearing from estimate:
                    Eigen::Vector3f estimated_landmark_position = estimated_state.segment(3 + 3 * state_map_it->second, 3);
                    //std::cout << "*** KNOWN LANDMARK FOUND ***" << std::endl;
                    //std::cout << "\t> estimated_landmark_position: " << estimated_landmark_position.transpose() << std::endl;
                    //std::cout << "\t> landmarks[meas.gt_landmark_id].position: " << landmarks[meas.gt_landmark_id].position.transpose() << std::endl;
                    Eigen::Vector3f estimated_landmark_position_rf_robot = estimated_rotation_world2robot_3f *
                        (estimated_landmark_position - Eigen::Vector3f(estimated_state[0], estimated_state[1], 0.0f));
                    Eigen::Vector3f estimated_landmark_position_rf_camera = (camera.transform_rf_parent.inverse() *
                                                                            Eigen::Vector4f(estimated_landmark_position_rf_robot.x(),
                                                                                            estimated_landmark_position_rf_robot.y(),
                                                                                            estimated_landmark_position_rf_robot.z(),
                                                                                            1.0f)).head(3);
                    Eigen::Vector3f Kpcam = camera.matrix * estimated_landmark_position_rf_camera;
                    estimated_uv(2 * k)     = Kpcam.x() / Kpcam.z();
                    estimated_uv(2 * k + 1) = Kpcam.y() / Kpcam.z();

                    // Compute bearing from measurement (using simulated depth sensor):
                    measured_uv(2 * k)     = meas.u;
                    measured_uv(2 * k + 1) = meas.v;

                    //std::cout << "estimated_uv: " << estimated_uv(2 * k) << ", " << estimated_uv(2 * k + 1)
                    //    << " - measured_uv: "<< measured_uv(2 * k) << ", " << measured_uv(2 * k + 1) << std::endl;

                    // Compute partial derivative of the landmark express in
                    // RF robot wrt robot (in rf world):
                    Eigen::Matrix<float, 3, 3> derivative_landmark_wrt_robot_rf_robot;
                    derivative_landmark_wrt_robot_rf_robot.block<3, 2>(0, 0) = -estimated_rotation_world2robot_3f.block<3, 2>(0, 0);
                    derivative_landmark_wrt_robot_rf_robot.block<3, 1>(0, 2) = estimated_derivative_transpose_rotation_world2robot * (estimated_landmark_position - Eigen::Vector3f(estimated_state[0], estimated_state[1], 0.0f));

                    // Compute partial derivative of proj wrt p=KR^T(x_t - t_t), K camera matrix:
                    Eigen::Matrix<float, 2, 3> derivative_proj_wrt_Kpcam;
                    derivative_proj_wrt_Kpcam << 1.0f / Kpcam.z(), 0.0f, -Kpcam.x() / std::pow(Kpcam.z(), 2.0f),
                                                 0.0f, 1.0f / Kpcam.z(), -Kpcam.y() / std::pow(Kpcam.z(), 2.0f);

                    // Compute partial derivative of proj wrt state (both robot and landmark):
                    jacobian_measurements.block<2, 3>(2 * k, 0) = derivative_proj_wrt_Kpcam * camera.matrix * rotation_robot2camera * derivative_landmark_wrt_robot_rf_robot;
                    jacobian_measurements.block<2, 3>(2 * k, 3 + 3 * state_map_it->second) = derivative_proj_wrt_Kpcam * camera.matrix * rotation_robot2camera * estimated_rotation_world2robot_3f;
                    ++k;
                }
            }

            if (tot_measurements_known_landmarks > 0) {
                const float measurement_noise = 12.0f;
                const Eigen::MatrixXf covariance_measurements = measurement_noise * Eigen::MatrixXf::Identity(2 * tot_measurements_known_landmarks, 2 * tot_measurements_known_landmarks);
                Eigen::MatrixXf kalman_gain_matrix = covariance_estimate * jacobian_measurements.transpose() * (jacobian_measurements * covariance_estimate * jacobian_measurements.transpose() + covariance_measurements).inverse();
                estimated_state.noalias() += kalman_gain_matrix * (measured_uv - estimated_uv);
                covariance_estimate = (Eigen::MatrixXf::Identity(estimated_state.rows(), estimated_state.rows()) - kalman_gain_matrix * jacobian_measurements) * covariance_estimate;
            }

            Eigen::Matrix4f estimated_transform_robot2world;
            estimated_transform_robot2world << std::cos(estimated_state[2]), -std::sin(estimated_state[2]), 0.0f, estimated_state[0],
                                               std::sin(estimated_state[2]),  std::cos(estimated_state[2]), 0.0f, estimated_state[1],
                                                                       0.0f,                          0.0f, 1.0f,               0.0f,
                                                                       0.0f,                          0.0f, 0.0f,               1.0f;

            // Update estimate_state and covariance_estimate if new landmarks
            // have been seen:
            const float initial_landmark_noise = 0.2f;
            estimated_state.conservativeResize(estimated_state.rows() + 3 * number_of_unknown_landmarks);
            covariance_estimate.conservativeResize(covariance_estimate.rows() + 3 * number_of_unknown_landmarks,
                                       covariance_estimate.cols() + 3 * number_of_unknown_landmarks);
            covariance_estimate.rightCols(3 * number_of_unknown_landmarks).setZero();
            covariance_estimate.bottomRows(3 * number_of_unknown_landmarks).setZero();
            covariance_estimate.block(covariance_estimate.rows() - 3 * number_of_unknown_landmarks,
                                      covariance_estimate.cols() - 3 * number_of_unknown_landmarks,
                                      3 * number_of_unknown_landmarks,
                                      3 * number_of_unknown_landmarks) = initial_landmark_noise * Eigen::MatrixXf::Identity(3 * number_of_unknown_landmarks, 3 * number_of_unknown_landmarks);
            k = 0;
            for (const auto& meas : measurements) {
                int meas_idx = &meas - &measurements[0];

                if (id_to_state_map.find(meas.gt_landmark_id) == id_to_state_map.end()) {
                    // Update maps:
                    id_to_state_map[meas.gt_landmark_id] = state_to_id_map.size();
                    state_to_id_map.push_back(meas.gt_landmark_id);

                    // Update estimated_state (covariance updated above):
                    Eigen::Vector3f landmark_initial_position = (estimated_transform_robot2world * gt_landmark_position_rf_robot_vector[meas_idx]).head(3);
                    std::cout << "Adding landmark id=" << meas.gt_landmark_id << " with initial diff " << (landmarks[meas.gt_landmark_id].position - landmark_initial_position).transpose() << std::endl;
                    std::cout << "\t>gt: " << landmarks[meas.gt_landmark_id].position.transpose() << std::endl;
                    std::cout << "\t>init: " << landmark_initial_position.transpose() << std::endl;
                    estimated_state.segment(estimated_state.rows()-3*(number_of_unknown_landmarks-k), 3) = landmark_initial_position;
                    ++k;
                }
            }
        }

        void least_squares(std::vector<Eigen::Vector3f>& estimated_trajectory,
                           std::vector<Eigen::Vector3f>& estimated_landmarks,
                           const std::vector<mcl::Measurement>& full_measurements,
                           const std::vector<std::pair<int, int>>& proj_pose_landmark_association,
                           //const std::vector<Eigen::Vector3f>& odometry_displacement,
                           const std::vector<mcl::Landmark>& landmarks,
                           const mcl::Camera& camera,
                           const int num_iterations,
                           const float damping,
                           const float kernel_threshold) {

            std::cout << "full_measurements.size(): " << full_measurements.size() << std::endl;
            std::cout << "proj_pose_landmark_association.size(): " << proj_pose_landmark_association.size() << std::endl;

            // Determine the size of the system Hdx+b=0:
            const int pose_dim = estimated_trajectory[0].rows(); // = 3 (x, y, theta)
            const int landmarks_dim = estimated_landmarks[0].rows(); // = 3 (x, y, z)
            const int system_size = pose_dim * estimated_trajectory.size() + landmarks_dim * estimated_landmarks.size();
            std::cout << "* system_size=" << system_size << std::endl;

            std::vector<Eigen::Vector3f> dx_pose_vector(estimated_trajectory.size()); // NOTE: pose_dim must be 3
            std::vector<Eigen::Vector3f> dx_landmark_vector(estimated_landmarks.size()); // NOTE: landmarks_dim must be 3

            // chi and inliers stats:
            std::vector<std::pair<float, int>> chi_inliers_stats;

            for (int iteration = 0; iteration < num_iterations; ++iteration) {
                std::cout << "LS iteration=" << iteration+1 << "/" << num_iterations << std::endl;
                int num_inliers = 0;
                float chi_tot = 0.0f;

                Eigen::MatrixXf H_projections = Eigen::MatrixXf::Zero(system_size, system_size);
                Eigen::VectorXf b_projections = Eigen::VectorXf::Zero(system_size);

                // 1a. Linearize proj()
                std::cout << "Linearize proj()." << std::endl;
                int meas_idx = 0;
                for (const auto& meas : full_measurements) {

                    // Compute derivative of proj(x+dx) wrt dx eval in dx=0:
                    std::cout << "\tComputing derivative of proj(x+dx) wrt dx eval in dx=0." << std::endl;
                    int pose_idx    = proj_pose_landmark_association[meas_idx].first;
                    int landmark_id = proj_pose_landmark_association[meas_idx].second;
                    const Eigen::Vector3f& curr_pose = estimated_trajectory[pose_idx];
                    Eigen::Vector4f curr_landmark;
                    curr_landmark.head<3>() = landmarks[landmark_id].position;
                    curr_landmark[3] = 1.0f; // homogeneous coords.
                    Eigen::Matrix4f T_robot2world = Eigen::Matrix4f::Identity();
                    T_robot2world.block<3, 3>(0, 0) = mcl::Rz(curr_pose[2]);
                    T_robot2world.block<2, 1>(0, 3) = curr_pose.head<2>();
                    const Eigen::Matrix4f T_world2camera = (T_robot2world * camera.transform_rf_parent).inverse();
                    //const Eigen::Matrix3f R_world2robot  = mcl::Rz(curr_pose[2]).transpose();
                    //const Eigen::Vector3f landmark_position_rf_robot = R_world2robot *
                    //    (landmarks[landmark_id].position - Eigen::Vector3f(curr_pose.x(), curr_pose.y(), 0.0f));
                    //const Eigen::Matrix3f R_robot2camera = camera.transform_rf_parent.block<3, 3>(0, 0).transpose();

                    // Position of the landmark in rf camera:
                    std::cout << "\tPosition of the landmark in rf camera." << std::endl;
                    Eigen::Vector3f pcam  = (T_world2camera * curr_landmark).head<3>();
                    Eigen::Vector3f Kpcam = camera.matrix * pcam;
                    Eigen::Vector2f proj_Kpcam(Kpcam.x() / Kpcam.z(),
                                               Kpcam.y() / Kpcam.z());

                    // Check that proj_Kpcam is feasible:
                    if (pcam.z() >= 0.0f && camera.is_valid(proj_Kpcam)) {
                        // *** Using chain rule: ***
                        // Derivative proj wrt Kpcam:
                        std::cout << "\tderivative_proj_wrt_Kpcam" << std::endl;
                        Eigen::Matrix<float, 2, 3> derivative_proj_wrt_Kpcam;
                        derivative_proj_wrt_Kpcam << 1.0f / Kpcam.z(), 0.0f, -Kpcam.x() / std::pow(Kpcam.z(), 2.0f),
                                                     0.0f, 1.0f / Kpcam.z(), -Kpcam.y() / std::pow(Kpcam.z(), 2.0f);

                        // Derivative Kpcam wrt dx (robot part x, y, theta):
                        std::cout << "\tderivative_Kpcam_wrt_dxr" << std::endl;
                        Eigen::Matrix3f derivative_Kpcam_wrt_dxr;
                        derivative_Kpcam_wrt_dxr << -1.0f,  0.0f,  curr_landmark.y(),
                                                     0.0f, -1.0f, -curr_landmark.x(),
                                                     0.0f,  0.0f,               0.0f;
                        derivative_Kpcam_wrt_dxr = camera.matrix * T_world2camera.block<3, 3>(0, 0) * derivative_Kpcam_wrt_dxr;

                        // Derivative Kpcam wrt dx (landmark part x, y, z):
                        std::cout << "\tderivative_Kpcam_wrt_dxl" << std::endl;
                        Eigen::Matrix3f derivative_Kpcam_wrt_dxl = camera.matrix * T_world2camera.block<3, 3>(0, 0);

                        // Apply kernel threshold before defining the Jacobians:
                        std::cout << "\tApplying kernel threshold." << std::endl;
                        Eigen::Vector2f projection_error = proj_Kpcam - Eigen::Vector2f(meas.u, meas.v);
                        float chi = projection_error.squaredNorm();
                        std::cout << "\tproj_Kpcam: " << proj_Kpcam.transpose() << std::endl;
                        std::cout << "\tmeas: " << meas.u << " " << meas.v << std::endl;
                        std::cout << "\tchi=" << chi << std::endl;

                        bool is_inlier = true;
                        if (chi > kernel_threshold) {
                            projection_error *= std::sqrt(kernel_threshold / chi);
                            chi = kernel_threshold;
                            is_inlier = false;
                        } else {
                            ++num_inliers;
                        }

                        chi_tot += chi;

                        // Jacobians (only on inliers):
                        if (is_inlier) {
                            std::cout << "\tBuilding jacobians." << std::endl;
                            Eigen::Matrix<float, 2, 3> Jr = derivative_proj_wrt_Kpcam * derivative_Kpcam_wrt_dxr;
                            Eigen::Matrix<float, 2, 3> Jl = derivative_proj_wrt_Kpcam * derivative_Kpcam_wrt_dxl;

                            int H_r_idx = pose_dim * pose_idx;
                            int H_l_idx = pose_dim * estimated_trajectory.size() + landmarks_dim * landmark_id;
                            std::cout << "\tpose_idx=" << pose_idx << std::endl;
                            std::cout << "\testimated_trajectory.size()=" <<  estimated_trajectory.size() << std::endl;
                            std::cout << "\tpose_dim=" << pose_dim << std::endl;
                            std::cout << "\tlandmark_id=" << landmark_id << std::endl;
                            std::cout << "\tlandmarks_dim=" << landmarks_dim << std::endl;
                            std::cout << "\tH_r_idx: " << H_r_idx << ", H_l_idx: " << H_l_idx << std::endl;
                            std::cout << "\tprojection_error=" << projection_error.transpose() << std::endl;

                            // Fill H and b (projections):
                            std::cout << "\tFilling H, b." << std::endl;
                            H_projections.block<3, 3>(H_r_idx, H_r_idx) += Jr.transpose() * Jr;
                            //H_projections.block<3, 3>(H_r_idx, H_l_idx) += Jr.transpose() * Jl;
                            //H_projections.block<3, 3>(H_l_idx, H_r_idx) += Jl.transpose() * Jr;
                            //H_projections.block<3, 3>(H_l_idx, H_l_idx) += Jl.transpose() * Jl;
                            b_projections.segment<3>(H_r_idx) += Jr.transpose() * projection_error;
                            //b_projections.segment<3>(H_l_idx) += Jl.transpose() * projection_error;
                        }
                    } else {
                        std::cout << "\tUNFEASIBLE POINT: "
                            << proj_Kpcam.transpose()
                            << " (z= " << pcam.z() << ")" << std::endl;
                    }

                    ++meas_idx;
                    std::cout << "\t-" << std::endl;
                }

                std::cout << "num_inliers: " << num_inliers << " ("
                    << 100.0f*num_inliers/full_measurements.size() << "%)" << std::endl;
                std::cout << "chi_tot: " << chi_tot << std::endl;

                // 1b. Linearize poses
                std::cout << "Linearize poses.\nTODO." << std::endl;
                // TODO.

                // 2. Build H, b
                std::cout << "Building H, b." << std::endl;
                Eigen::MatrixXf H = H_projections;
                Eigen::VectorXf b = b_projections;
                //H += H_projections;
                //b += b_projections;

                // Add dumping to H:
                //H += damping * Eigen::MatrixXf::Identity(system_size, system_size);

                // 3. Solve Hdeltax+b=0 (lock the last pose):
                std::cout << "Solving Hdeltax+b=0." << std::endl;
                Eigen::VectorXf dx = Eigen::VectorXf::Zero(system_size);
                const int subsys_size = system_size - pose_dim;
                dx.tail(subsys_size) = -H.bottomRightCorner(subsys_size, subsys_size).ldlt().solve(b.tail(subsys_size));

                // boxplus:
                std::cout << "boxplus" << std::endl;
                for (int dx_idx = 0; dx_idx < estimated_trajectory.size(); ++dx_idx) {
                    dx_pose_vector[dx_idx] = dx.segment(pose_dim * dx_idx, pose_dim);
                }
                for (int dx_idx = 0; dx_idx < estimated_landmarks.size(); ++dx_idx) {
                    dx_landmark_vector[dx_idx] = dx.segment(pose_dim * estimated_trajectory.size() + landmarks_dim * dx_idx, landmarks_dim);
                }
                std::cout << "iter_boxplus" << std::endl;
                iter_boxplus(estimated_trajectory, dx_pose_vector, boxplus_se2);
                //iter_boxplus(estimated_landmarks, dx_landmark_vector, boxplus_euclidean);

                // Update statistics:
                chi_inliers_stats.push_back(std::make_pair(chi_tot, num_inliers));
            }

            // Print statistics about chi and inliers for each iteration:
            std::cout << "*** CHI/INLIERS STATS ***" << std::endl;
            for (const auto& chi_inliers : chi_inliers_stats) {
                std::cout << std::setw(10)
                    << std::setprecision(3) << chi_inliers.first
                    << std::setw(10) << chi_inliers.second
                    << " (" << 100.0f*chi_inliers.second/full_measurements.size() << "%)"
                    << std::endl;
            }
        } // end least_squares()
    } // end namespace mcl::slam
} // end namespace mcl
