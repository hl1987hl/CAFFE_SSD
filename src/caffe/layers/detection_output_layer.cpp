#include <fstream>  // NOLINT(readability/streams)
#include <map>
#include <string>
#include <vector>

#include "boost/filesystem.hpp"

#include "caffe/layers/detection_output_layer.hpp"
#include "caffe/util/bbox_util.hpp"

namespace caffe {

template <typename Dtype>
void DetectionOutputLayer<Dtype>::LayerSetUp(const vector<Blob<Dtype>*>& bottom,
      const vector<Blob<Dtype>*>& top) {
  const DetectionOutputParameter& detection_output_param =
      this->layer_param_.detection_output_param();
  CHECK(detection_output_param.has_num_classes()) << "Must specify num_classes";
  num_classes_ = detection_output_param.num_classes();
  share_location_ = detection_output_param.share_location();
  loc_classes_ = share_location_ ? 1 : num_classes_;
  background_label_id_ = detection_output_param.background_label_id();
  // Parameters used in nms.
  nms_threshold_ = detection_output_param.nms_param().nms_threshold();
  CHECK_GE(nms_threshold_, 0.) << "nms_threshold must be non negative.";
  top_k_ = -1;
  if (detection_output_param.nms_param().has_top_k()) {
    top_k_ = detection_output_param.nms_param().top_k();
  }
  const SaveOutputParameter& save_output_param =
      detection_output_param.save_output_param();
  output_directory_ = save_output_param.output_directory();
  if (!output_directory_.empty() &&
      !boost::filesystem::is_directory(output_directory_)) {
    if (!boost::filesystem::create_directories(output_directory_)) {
        LOG(FATAL) << "Failed to create directory: " << output_directory_;
    }
  }
  output_name_prefix_ = save_output_param.output_name_prefix();
  need_save_ = output_directory_ == "" ? false : true;
  output_format_ = save_output_param.output_format();
  if (output_format_ == "VOC") {
    string label_map_file = save_output_param.label_map_file();
    if (label_map_file.empty()) {
      // Ignore saving if there is no label_map_file provided for VOC output.
      LOG(WARNING) << "Provide label_map_file if output results for VOC.";
      need_save_ = false;
    } else {
      LabelMap label_map;
      CHECK(ReadProtoFromTextFile(label_map_file, &label_map))
          << "Failed to read label map file: " << label_map_file;
      CHECK(MapLabelToName(label_map, true, &label_to_name_))
          << "Failed to convert label to name.";
    }
    string name_size_file = save_output_param.name_size_file();
    if (name_size_file.empty()) {
      // Ignore saving if there is no name_size_file provided for VOC output.
      LOG(WARNING) << "Provide name_size_file if output results for VOC.";
      need_save_ = false;
    } else {
      std::ifstream infile(name_size_file.c_str());
      CHECK(infile.good())
          << "Failed to open name size file: " << name_size_file;
      // The file is in the following format:
      //    name height width
      //    ...
      string name;
      int height, width;
      while (infile >> name >> height >> width) {
        // Get the basename without extension of the filename
        names_.push_back(name);
        sizes_.push_back(std::make_pair(height, width));
      }
      infile.close();
      name_count_ = 0;
    }
    // Clean all output files.
    if (need_save_) {
      boost::filesystem::path output_directory(output_directory_);
      for (map<int, string>::iterator it = label_to_name_.begin();
           it != label_to_name_.end(); ++it) {
        if (it->first == background_label_id_) {
          continue;
        }
        std::ofstream outfile;
        boost::filesystem::path file(
            output_name_prefix_ + it->second + ".txt");
        boost::filesystem::path out_file = output_directory / file;
        outfile.open(out_file.string().c_str(), std::ofstream::out);
      }
    }
  }
}

template <typename Dtype>
void DetectionOutputLayer<Dtype>::Reshape(const vector<Blob<Dtype>*>& bottom,
      const vector<Blob<Dtype>*>& top) {
  CHECK_EQ(bottom[0]->num(), bottom[1]->num());
  num_priors_ = bottom[2]->height() / 4;
  CHECK_EQ(num_priors_ * loc_classes_ * 4, bottom[0]->channels())
      << "Number of priors must match number of location predictions.";
  CHECK_EQ(num_priors_ * num_classes_, bottom[1]->channels())
      << "Number of priors must match number of confidence predictions.";
  // num() and channels() are 1.
  vector<int> top_shape(2, 1);
  // Since the number of bboxes to be kept is unknown before nms, we manually
  // set it to (fake) 1.
  top_shape.push_back(1);
  // Each row is a 7 dimension vector, which stores
  // [image_id, label, confidence, xmin, ymin, xmax, ymax]
  top_shape.push_back(7);
  top[0]->Reshape(top_shape);
}

template <typename Dtype>
void DetectionOutputLayer<Dtype>::Forward_cpu(
    const vector<Blob<Dtype>*>& bottom, const vector<Blob<Dtype>*>& top) {
  const Dtype* loc_data = bottom[0]->cpu_data();
  const Dtype* conf_data = bottom[1]->cpu_data();
  const Dtype* prior_data = bottom[2]->cpu_data();
  const int num = bottom[0]->num();

  // Retrieve all location predictions.
  vector<LabelBBox> all_loc_preds;
  GetLocPredictions(loc_data, num, num_priors_, loc_classes_, share_location_,
                    &all_loc_preds);

  // Retrieve all confidences.
  vector<map<int, vector<float> > > all_conf_scores;
  GetConfidenceScores(conf_data, num, num_priors_, num_classes_,
                      &all_conf_scores);

  // Retrieve all prior bboxes. It is same within a batch since we assume all
  // images in a batch are of same dimension.
  vector<NormalizedBBox> prior_bboxes;
  vector<vector<float> > prior_variances;
  GetPriorBBoxes(prior_data, num_priors_, &prior_bboxes, &prior_variances);

  int num_kept = 0;
  vector<map<int, vector<int> > > all_indices;
  vector<LabelBBox> all_decode_bboxes;
  for (int i = 0; i < num; ++i) {
    // Decode predictions into bboxes.
    LabelBBox decode_bboxes;
    for (int c = 0; c < loc_classes_; ++c) {
      int label = share_location_ ? -1 : c;
      if (label == background_label_id_) {
        // Ignore background class.
        continue;
      }
      if (all_loc_preds[i].find(label) == all_loc_preds[i].end()) {
        // Something bad happened if there are no predictions for current label.
        LOG(FATAL) << "Could not find location predictions for label " << label;
      }
      DecodeBBoxes(prior_bboxes, prior_variances, all_loc_preds[i][label],
                   &(decode_bboxes[label]));
    }
    all_decode_bboxes.push_back(decode_bboxes);

    // For each class, perform nms
    map<int, vector<int> > indices;
    map<int, map<int, map<int, float> > > overlaps;
    for (int c = 0; c < num_classes_; ++c) {
      if (c == background_label_id_) {
        // Ignore background class.
        continue;
      }
      if (all_conf_scores[i].find(c) == all_conf_scores[i].end()) {
        // Something bad happened if there are no predictions for current label.
        LOG(FATAL) << "Could not find confidence predictions for label " << c;
      }
      int label = share_location_ ? -1 : c;
      if (decode_bboxes.find(label) == decode_bboxes.end()) {
        // Something bad happened if there are no predictions for current label.
        LOG(FATAL) << "Could not find location predictions for label " << label;
        continue;
      }
      ApplyNMS(decode_bboxes[label], all_conf_scores[i][c], nms_threshold_,
               top_k_, share_location_, &(overlaps[label]), &(indices[c]));
      num_kept += indices[c].size();
    }
    all_indices.push_back(indices);
  }

  vector<int> top_shape(2, 1);
  top_shape.push_back(num_kept);
  top_shape.push_back(7);
  top[0]->Reshape(top_shape);
  Dtype* top_data = top[0]->mutable_cpu_data();

  int count = 0;
  boost::filesystem::path output_directory(output_directory_);
  for (int i = 0; i < num; ++i) {
    const LabelBBox& decode_bboxes = all_decode_bboxes[i];
    for (map<int, vector<int> >::iterator it = all_indices[i].begin();
         it != all_indices[i].end(); ++it) {
      int label = it->first;
      int loc_label = share_location_ ? -1 : label;
      if (decode_bboxes.find(loc_label) == decode_bboxes.end()) {
        // Something bad happened if there are no predictions for current label.
        LOG(FATAL) << "Could not find location predictions for " << loc_label;
        continue;
      }
      const vector<NormalizedBBox>& bboxes =
          decode_bboxes.find(loc_label)->second;
      vector<int>& indices = it->second;
      std::ofstream outfile;
      if (need_save_) {
        CHECK(label_to_name_.find(label) != label_to_name_.end())
            << "Cannot find label: " << label << " in the label map.";
        boost::filesystem::path file(
            output_name_prefix_ + label_to_name_[label] + ".txt");
        boost::filesystem::path out_file = output_directory / file;
        outfile.open(out_file.string().c_str(),
                     std::ofstream::out | std::ofstream::app);
        CHECK_LT(name_count_, names_.size());
      }
      for (int j = 0; j < indices.size(); ++j) {
        int idx = indices[j];
        top_data[count * 7] = i;
        top_data[count * 7 + 1] = label;
        top_data[count * 7 + 2] = all_conf_scores[i][label][idx];
        NormalizedBBox clip_bbox;
        ClipBBox(bboxes[idx], &clip_bbox);
        top_data[count * 7 + 3] = clip_bbox.xmin();
        top_data[count * 7 + 4] = clip_bbox.ymin();
        top_data[count * 7 + 5] = clip_bbox.xmax();
        top_data[count * 7 + 6] = clip_bbox.ymax();
        if (need_save_) {
          outfile << names_[name_count_];
          outfile << " " << all_conf_scores[i][label][idx];
          NormalizedBBox scale_bbox;
          ScaleBBox(clip_bbox, sizes_[name_count_].first,
                    sizes_[name_count_].second, &scale_bbox);
          outfile << " " << static_cast<int>(scale_bbox.xmin());
          outfile << " " << static_cast<int>(scale_bbox.ymin());
          outfile << " " << static_cast<int>(scale_bbox.xmax());
          outfile << " " << static_cast<int>(scale_bbox.ymax());
          outfile << std::endl;
          outfile.flush();
        }
        ++count;
      }
      if (need_save_) {
        outfile.close();
      }
    }
    if (need_save_) {
      ++name_count_;
    }
  }
}

INSTANTIATE_CLASS(DetectionOutputLayer);
REGISTER_LAYER_CLASS(DetectionOutput);

}  // namespace caffe