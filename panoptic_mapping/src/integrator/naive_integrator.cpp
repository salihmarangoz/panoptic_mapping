#include "panoptic_mapping/integrator/naive_integrator.h"

namespace panoptic_mapping {

void NaiveIntegrator::setupFromConfig(IntegratorBase::Config *config) {
  CHECK_NOTNULL(config);
  auto cfg = dynamic_cast<Config *>(config);
  if (cfg) {
    config_ = *cfg;
  } else {
    LOG(ERROR) << "Failed to setup: config is not of type 'NaivePointcloudIntegrator::Config'.";
  }
}

void NaiveIntegrator::processPointcloud(SubmapCollection *submaps,
                                        const Transformation &T_M_C,
                                        const Pointcloud &pointcloud,
                                        const Colors &colors,
                                        const std::vector<int> &ids) {
  CHECK_NOTNULL(submaps);
  CHECK_EQ(ids.size(), pointcloud.size());
  CHECK_EQ(ids.size(), colors.size());

  // Segment the pointcloud by id
  std::vector<voxblox::Pointcloud> cloud_v;
  std::vector<voxblox::Colors> color_v;
  std::vector<int> id_v;

  for (size_t i = 0; i < ids.size(); ++i) {
    auto it = std::find(id_v.begin(), id_v.end(), ids[i]);
    size_t index = it - id_v.begin();
    if (it == id_v.end()) {
      // allocate new partial cloud
      id_v.emplace_back(ids[i]);
      cloud_v.emplace_back(voxblox::Pointcloud());
      color_v.emplace_back(voxblox::Colors());
    }
    cloud_v.at(index).push_back(pointcloud[i]);
    color_v.at(index).push_back(colors[i]);
  }

  // integrate each partial pointcloud to each submap
  for (size_t i = 0; i < id_v.size(); ++i) {
    if (!submaps->submapIdExists(id_v[i])) {
      // all submaps should already be allocated
      LOG(WARNING) << "Failed to integrate pointcloud to submap with ID '" << id_v[i] << "': submap does not exist.";
      continue;
    }
    if (!tsdf_integrator_) {
      // We just use the fast integrator here, could also add others
      tsdf_integrator_ =
          voxblox::TsdfIntegratorFactory::create(config_.voxblox_integrator_type,
                                                 config_.voxblox_integrator_config,
                                                 submaps->getSubmap(id_v[i]).getTsdfLayerPtr());
    } else {
      tsdf_integrator_->setLayer(submaps->getSubmap(id_v[i]).getTsdfLayerPtr());
    }
    tsdf_integrator_->integratePointCloud(T_M_C, cloud_v[i], color_v[i]);
  }
}

}  // namespace panoptic_mapping