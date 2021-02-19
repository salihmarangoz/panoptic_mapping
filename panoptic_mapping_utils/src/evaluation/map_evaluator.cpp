#include "panoptic_mapping_utils/evaluation/map_evaluator.h"

#include <cmath>
#include <string>
#include <memory>
#include <vector>
#include <algorithm>

#include <pcl/io/ply_io.h>
#include <ros/ros.h>
#include <voxblox/interpolator/interpolator.h>

#include "panoptic_mapping_utils/evaluation/progress_bar.h"

namespace panoptic_mapping {

void MapEvaluator::EvaluationRequest::checkParams() const {
  checkParamGT(maximum_distance, 0.f, "maximum_distance");
}

void MapEvaluator::EvaluationRequest::setupParamsAndPrinting() {
  setupParam("verbosity", &verbosity);
  setupParam("map_file", &map_file);
  setupParam("ground_truth_pointcloud_file", &ground_truth_pointcloud_file);
  setupParam("maximum_distance", &maximum_distance);
  setupParam("evaluate", &evaluate);
  setupParam("visualize", &visualize);
  setupParam("compute_coloring", &compute_coloring);
}

MapEvaluator::MapEvaluator(const ros::NodeHandle& nh,
                           const ros::NodeHandle& nh_private)
    : nh_(nh), nh_private_(nh_private) {
  visualizer_ = std::make_unique<SubmapVisualizer>(
      config_utilities::getConfigFromRos<SubmapVisualizer::Config>(nh_private_),
      std::make_shared<LabelHandler>());
}

bool MapEvaluator::evaluate(const EvaluationRequest& request) {
  if (!request.isValid(true)) {
    return false;
  }
  LOG_IF(INFO, request.verbosity >= 2) << "Processing: \n"
                                       << request.toString();

  // Load the groundtruth pointcloud.
  if (request.evaluate || request.compute_coloring) {
    if (!request.ground_truth_pointcloud_file.empty()) {
      gt_ptcloud_ = std::make_unique<pcl::PointCloud<pcl::PointXYZ>>();
      if (pcl::io::loadPLYFile<pcl::PointXYZ>(
              request.ground_truth_pointcloud_file, *gt_ptcloud_) != 0) {
        LOG(ERROR) << "Could not load ground truth point cloud from '"
                   << request.ground_truth_pointcloud_file << "'.";
        gt_ptcloud_.reset();
        return false;
      }
      LOG_IF(INFO, request.verbosity >= 2) << "Loaded ground truth pointcloud";
    }
    if (!gt_ptcloud_) {
      LOG(ERROR) << "No ground truth pointcloud loaded.";
      return false;
    }
  }
  if (request.visualize || request.evaluate || request.compute_coloring) {
    // Load the map to evaluate.
    if (!request.map_file.empty()) {
      submaps_ = std::make_shared<SubmapCollection>();
      if (!submaps_->loadFromFile(request.map_file)) {
        LOG(ERROR) << "Could not load ground truth point cloud from '"
                   << request.ground_truth_pointcloud_file << "'.";
        submaps_.reset();
        return false;
      }

      // Setup related tools.
      planning_ = std::make_unique<PlanningInterface>(submaps_);
      size_t separator = request.map_file.find_last_of('/');
      target_directory_ = request.map_file.substr(0, separator);
      target_map_name_ = request.map_file.substr(
          separator + 1, request.map_file.length() - separator - 8);
      LOG_IF(INFO, request.verbosity >= 2) << "Loaded the target panoptic map.";
    }
    if (!submaps_) {
      LOG(ERROR) << "No panoptic map loaded.";
      return false;
    }
  }

  // Setup output file
  if (request.evaluate) {
    std::string out_file_name =
        target_directory_ + "/" + target_map_name_ + "_evaluation_data.csv";
    output_file_.open(out_file_name, std::ios::out);
    if (!output_file_.is_open()) {
      LOG(ERROR) << "Failed to open output file '" << out_file_name << "'.";
      return false;
    }

    // Evaluate.
    LOG_IF(INFO, request.verbosity >= 2) << "Computing reconstruction error:";
    computeReconstructionError(request);
    output_file_.close();
  }

  // Compute visualization if required.
  if (request.compute_coloring) {
    LOG_IF(INFO, request.verbosity >= 2) << "Computing visualization coloring:";
    visualizeReconstructionError(request);
  }

  // Display the mesh.
  if (request.visualize) {
    LOG_IF(INFO, request.verbosity >= 2) << "Publishing mesh.";
    publishVisualization();
  }

  LOG_IF(INFO, request.verbosity >= 2) << "Done.";
  return true;
}

void MapEvaluator::computeReconstructionError(
    const EvaluationRequest& request) {
  // Go through each point, use trilateral interpolation to figure out the
  // distance at that point.
  std::unique_ptr<Bounds> bounds = std::make_unique<FlatBounds>();

  // Setup.
  const uint64_t total_points = gt_ptcloud_->size();
  uint64_t unknown_points = 0;
  uint64_t truncated_points = 0;
  std::vector<float> abserror;
  abserror.reserve(total_points);  // Just reserve the worst case.

  // Setup progress bar.
  const uint64_t interval = total_points / 100;
  uint64_t count = 0;
  ProgressBar bar;

  // Evaluate gt pcl based(# gt points within < trunc_dist)
  for (const auto& pcl_point : *gt_ptcloud_) {
    const voxblox::Point point(pcl_point.x, pcl_point.y, pcl_point.z);
    if (!bounds->pointIsValid(point)) {
      continue;
    }
    float distance;

    if (planning_->getDistance(point, &distance)) {
      if (std::abs(distance) > request.maximum_distance) {
        distance = request.maximum_distance;
        truncated_points++;
      }
      abserror.push_back(std::abs(distance));
    } else {
      unknown_points++;
    }

    // Progress bar.
    if (count % interval == 0) {
      bar.display(static_cast<float>(count) / total_points);
    }
    count++;
  }
  bar.display(100.f);

  // Report summary.
  float mean = 0.0;
  float rmse = 0.0;
  for (auto value : abserror) {
    mean += value;
    rmse += std::pow(value, 2);
  }
  if (!abserror.empty()) {
    mean /= static_cast<float>(abserror.size());
    rmse = std::sqrt(rmse / static_cast<float>(abserror.size()));
  }
  float stddev = 0.0;
  for (auto value : abserror) {
    stddev += std::pow(value - mean, 2.0);
  }
  if (abserror.size() > 2) {
    stddev = sqrt(stddev / static_cast<float>(abserror.size() - 1));
  }

  output_file_ << "MeanError [m],StdError [m],RMSE [m],TotalPoints [1],"
               << "UnknownPoints [1],TruncatedPoints [1]\n";
  output_file_ << mean << "," << stddev << "," << rmse << "," << total_points
               << "," << unknown_points << "," << truncated_points << "\n";
}

void MapEvaluator::computeErrorHistogram(const EvaluationRequest& request) {
  // create histogram of error distribution
  //    std::vector<int> histogram(hist_bins_);
  //    float bin_size = truncation_distance / ((float) hist_bins_ - 1.0);
  //    for (int i = 0; i < abserror.size(); ++i) {
  //      int bin = (int) floor(abserror[i] / bin_size);
  //      if (bin < 0 || bin >= hist_bins_) {
  //        std::cout << "Bin Error at bin " << bin << ", value " << abserror[i]
  //                  << std::endl;
  //        continue;
  //      }
  //      histogram[bin] += 1;
  //    }
  //    hist_file_ << map_name;
  //    for (int i = 0; i < histogram.size(); ++i) {
  //      hist_file_ << "," << histogram[i];
  //    }
  //    hist_file_ << "\n";
}

void MapEvaluator::visualizeReconstructionError(
    const EvaluationRequest& request) {
  // Coloring: grey -> unknown, green -> 0 error, red -> maximum error,
  // purple -> truncated to max error.

  constexpr int max_number_of_neighbors =
      100;  // max eval points for faster lookup.
  std::unique_ptr<Bounds> bounds = std::make_unique<FlatBounds>();

  // Build a Kd tree for point lookup.
  TreeData kdtree_data;
  kdtree_data.points.reserve(gt_ptcloud_->size());
  for (const auto& point : *gt_ptcloud_) {
    kdtree_data.points.emplace_back(point.x, point.y, point.z);
  }
  KDTree kdtree(3, kdtree_data, nanoflann::KDTreeSingleIndexAdaptorParams(10));
  kdtree.buildIndex();

  // Setup progress bar.
  float counter = 0.f;
  float max_counter = 0.f;
  ProgressBar bar;
  for (auto& submap : *submaps_) {
    voxblox::BlockIndexList block_list;
    submap->getTsdfLayer().getAllAllocatedBlocks(&block_list);
    max_counter += block_list.size();
  }

  // Parse all submaps
  for (auto& submap : *submaps_) {
    const size_t num_voxels_per_block =
        std::pow(submap->getTsdfLayer().voxels_per_side(), 3);
    const float voxel_size = submap->getTsdfLayer().voxel_size();
    const float voxel_size_sqr = voxel_size * voxel_size;
    const float truncation_distance = submap->getConfig().truncation_distance;
    voxblox::Interpolator<TsdfVoxel> interpolator(
        submap->getTsdfLayerPtr().get());

    // Parse all voxels.
    voxblox::BlockIndexList block_list;
    submap->getTsdfLayer().getAllAllocatedBlocks(&block_list);
    for (auto& block_index : block_list) {
      voxblox::Block<TsdfVoxel>& block =
          submap->getTsdfLayerPtr()->getBlockByIndex(block_index);
      for (size_t linear_index = 0; linear_index < num_voxels_per_block;
           ++linear_index) {
        TsdfVoxel& voxel = block.getVoxelByLinearIndex(linear_index);
        if (voxel.distance > truncation_distance ||
            voxel.distance < -truncation_distance) {
          continue;  // these voxels can never be surface.
        }
        Point center = block.computeCoordinatesFromLinearIndex(linear_index);
        if (!bounds->pointIsValid(center)) {
          // Out of bounds.
          voxel.color = Color(128, 128, 128);
          continue;
        }

        // Find surface points within 1 voxel size.
        // Note(schmluk): Use N neighbor search wih increasing N since radius
        // search is ridiculously slow.
        float query_pt[3] = {center.x(), center.y(), center.z()};
        std::vector<size_t> ret_index(max_number_of_neighbors);
        std::vector<float> out_dist_sqr(max_number_of_neighbors);
        int num_results =
            kdtree.knnSearch(&query_pt[0], max_number_of_neighbors,
                             &ret_index[0], &out_dist_sqr[0]);

        if (num_results == 0) {
          // No nearby surface.
          voxel.color = Color(128, 128, 128);
          continue;
        }

        // Get average error.
        float total_error = 0.f;
        int counted_voxels = 0;
        for (int i = 0; i < num_results; ++i) {
          if (i != 0 && out_dist_sqr[i] > voxel_size_sqr) {
            continue;
          }
          voxblox::FloatingPoint distance;
          if (interpolator.getDistance(kdtree_data.points[ret_index[i]],
                                       &distance, true)) {
            total_error += std::abs(distance);
            counted_voxels++;
          }
        }
        // Coloring.
        if (counted_voxels == 0) {
          voxel.color = Color(128, 128, 128);
        } else {
          float frac =
              std::min(total_error / counted_voxels, request.maximum_distance) /
              request.maximum_distance;
          float r = std::min((frac - 0.5f) * 2.f + 1.f, 1.f) * 255.f;
          float g = (1.f - frac) * 2.f * 255.f;
          if (frac <= 0.5f) {
            g = 190.f + 130.f * frac;
          }
          voxel.color = voxblox::Color(r, g, 0);
        }
      }

      // Show progress.
      counter += 1.f;
      bar.display(counter / max_counter);
    }
    submap->updateMesh(false);
  }

  // Store colored submaps.
  submaps_->saveToFile(target_directory_ + "/" + target_map_name_ +
                       "_evaluated.panmap");
}

void MapEvaluator::publishVisualization() {
  // Make sure the tfs arrive otherwise the mesh will be discarded.
  visualizer_->reset();
  visualizer_->visualizeAll(*submaps_);
}

}  // namespace panoptic_mapping
