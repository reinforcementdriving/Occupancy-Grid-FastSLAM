//
//  RBPF.cpp
//  FastSLAM
//
//  Created by Mats Steinweg on 20.08.19.
//  Copyright © 2019 Mats Steinweg. All rights reserved.
//

#include <stdio.h>
#include <random>
#include <math.h>
#include <algorithm>

#include "RBPF.h"
#include "Robot.h"

using namespace std;

#define PI 3.14159265


// +++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
// ++++++++++++++++++++++++++++++++++++++++++++ Constructor ++++++++++++++++++++++++++++++++++++++++++++++++++
// +++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

// Standard constructor
RBPF::RBPF(){
    
    this->n_particles = 5;
    this->R(0) = 0.01;
    this->R(1) = 0.01;
    this->R(2) = 0.01;
    
    // Initialize particles
    for (int i = 0; i < this->n_particles; i++) {
        Particle particle = Particle();
        this->particles.push_back(particle);
    }
}

// Constructor
RBPF::RBPF(int n_particles, Eigen::Vector3f R, int max_iterations, float tolerance, float discard_fraction): scan_matcher(max_iterations, tolerance, discard_fraction){
    
    this->n_particles = n_particles;
    this->R(0) = R(0);
    this->R(1) = R(1);
    this->R(2) = R(2);
    
    // Initialize particles
    for (int i = 0; i < this->n_particles; i++) {
        Particle particle = Particle();
        this->particles.push_back(particle);
    }
}


// +++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
// ++++++++++++++++++++++++++++++++++++++++++++++ Print Summary ++++++++++++++++++++++++++++++++++++++++++++++
// +++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

void RBPF::summary(){
    
    cout << "RBPF:" << endl;
    cout << "-----" << endl;
    cout << "Number of Particles: " << this->n_particles << endl;
    cout << "Motion Uncertainty: " << this->R(0) << "m, " << this->R(1) << "m, " << this->R(2) << "rad" << endl;
    // Print scan matcher summary
    this->getScanMatcher().summary();
    
}


// +++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
// ++++++++++++++++++++++++++++++++++++++++++++++ Run filter +++++++++++++++++++++++++++++++++++++++++++++++++
// +++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

void RBPF::run(Robot& robot, const int simulation_mode){
    
    // For localization and SLAM update particle poses
    if (simulation_mode == 0 || simulation_mode == 2){
        
        // Compute prediction based on odometry information and motion model
        this->predict(robot.getV(), robot.getOmega(), robot.getTimestamp());
        
        // Get sensor scan estimate
        this->sweep_estimate(robot.getSensor());
            
        // Run scan matching to compute pose correction
        this->scan_matching(robot.getPose(), robot.getSensor());
        
        // Weight particles according to measurement model
        this->weight(robot.getSensor());
        
        // Resample particles based on computed weights
        this->resample();
        
    }
    // For mapping set particle poses to current robot pose (mapping with known poses)
    else {
        
        for (list<Particle>::iterator it = this->particles.begin(); it != this->particles.end(); it++) {
            (*it).getPose() = robot.getPose();
        }
    }
    
    // For mapping and SLAM construct map from current sensor readings
    if (simulation_mode == 1 || simulation_mode == 2){
        this->mapping(robot.getSensor());
    }
}


// +++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
// ++++++++++++++++++++++++++++++++++++++++++++++ Prediction +++++++++++++++++++++++++++++++++++++++++++++++++
// +++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

// Apply motion model based on odometry information from wheel encoder
void RBPF::predict(const float &v, const float &omega, const float& current_timestamp){
    
    // Get sampling time
    float delta_t = current_timestamp - this->last_timestamp;
    
    // Create standard normal distribution for sampling
    default_random_engine generator;
    normal_distribution<float> distribution(0.0, 1.0);
    
    // Iterate over all particles
    for (list<Particle>::iterator it = this->particles.begin(); it != this->particles.end(); it++) {
        
        // Pose difference due to control signal since last update
        Eigen::Vector3f pose_dif;
        pose_dif(0) = delta_t * v * cos((float)(*it).getPose()(2));
        pose_dif(1) = delta_t * v * sin((float)(*it).getPose()(2));
        pose_dif(2) = delta_t * omega;
        
        // Update pose
        (*it).getPose() += pose_dif;
        
        // Apply uncorrelated noise
        (*it).getPose()(0) += (this->R(0) * distribution(generator));
        (*it).getPose()(1) += (this->R(1) * distribution(generator));
        (*it).getPose()(2) += (this->R(2) * distribution(generator));
        // Limit heading to range [-pi, pi)
        (*it).getPose()(2) = fmod((float)(*it).getPose()(2)+PI, 2*PI) - PI;
                
    }
    
    // Update timestamp
    this->last_timestamp = current_timestamp;
    
}


// +++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
// +++++++++++++++++++++++++++++++++++++++++++ Scan Estimate +++++++++++++++++++++++++++++++++++++++++++++++++
// +++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

// Estimate sensor sweep from particles' maps
void RBPF::sweep_estimate(Sensor &sensor){
    
    // Iterate over all particles
    for (list<Particle>::iterator it = this->particles.begin(); it != this->particles.end(); it++) {
        
        // Transform robot pose from world coordinates to map coordinates
        Eigen::Array3f map_pose = Map::world2map((*it).getPose());

        // Get mass center of robot's center pixel
        const float xc_r = (float) map_pose(0) + 0.5;
        const float yc_r = (float) map_pose(1) + 0.5;
        const float heading_r = (float) map_pose(2);
        
        // Transform sensor's maximum range to map scale
        int map_range = Map::world2map(sensor.getRange());
        
        // Instantiate container for estimated measurements
        (*it).getMeasurementEstimate() = Eigen::MatrixX2f::Ones(sensor.getN(), 2) * sensor.getRange(); // Initialize measurements to sensor range
        (*it).getMeasurementEstimate().col(0) = Eigen::ArrayXf::LinSpaced(sensor.getN(), -PI/180*sensor.getFoV()/2, PI/180*sensor.getFoV()/2); // Set angle for each laser beam
        
        // Get reference to real laser measurements
        Eigen::MatrixX2f measurement_ref = sensor.getMeasurements();
        
        // Get reference to the currently inspected particle's map data
        cv::Mat map_ref = (*it).getMap().getData();
        
        // Iterate over all vertical pixels within range of the sensor from robot's current position
        for (int y_px = ((int)map_pose(1) - map_range); y_px <= ((int)(map_pose(1) + map_range)); y_px++) {
            // Check that index lies within map boundaries
            if (y_px >= 0 && y_px < (int)(Map::getHeight() / Map::getResolution())) {
                
                // Get a pointer to the beginning of the currently inspected row to access all relevant
                // horizontal pixels
                uchar* horizontal_pixel_ptr = map_ref.ptr<uchar>(y_px);
                
                // Iterate over all horizontal pixels within range of the sensor from robot's current position
                for (int x_px = ((int)map_pose(0) - map_range); x_px <= ((int)(map_pose(0) + map_range)); x_px++) {
                    // Check that index lies within map boundaries
                    if (x_px >= 0 && x_px < (int)(Map::getWidth() / Map::getResolution())) {
                    
                        // Update estimated scan only if occupied cell detected
                        if (horizontal_pixel_ptr[x_px] < Map::getThreshold()) {
                            
                            // Get mass center of currently inspected pixel
                            const float xc_px = x_px + 0.5;
                            const float yc_px = y_px + 0.5;
                            
                            // Compute relative angles of all corner points of currently inspected pixel
                            vector<float> pixel_angles;
                            for (float i = -0.5; i <= 0.5; i+=0.5) {
                                for (float j = -0.5; j<= 0.5; j+=0.5) {
                                    
                                    // Angle of currently inspected pixel relative to robot's heading
                                    float pixel_angle = atan2((yc_px+j) - yc_r, (xc_px+i) - xc_r) - heading_r;
                                    // Keep angle within range [-pi, pi)
                                    if (pixel_angle < -PI) {
                                        pixel_angle = fmod(pixel_angle-PI, 2*PI) + PI; }
                                    else {
                                        pixel_angle = fmod(pixel_angle+PI, 2*PI) - PI; }
                                    
                                    pixel_angles.push_back(pixel_angle);
                                }
                            }
                            
                            // Get minimum and maximum relative angle that hits currently inspected pixel
                            float angle_max = *max_element(pixel_angles.begin(), pixel_angles.end());
                            float angle_min = *min_element(pixel_angles.begin(), pixel_angles.end());
                            
                            // Distance from robot to mass center of currently inspected pixel
                            const float pixel_distance = sqrt(pow(xc_px - xc_r, 2) + pow(yc_px - yc_r, 2));
                            
                            // Find all relevant laser beams
                            vector <int> valid_ids;
                            if (angle_min < -PI/2 && angle_max > PI/2) {
                                for (int beam_id = 0; beam_id < sensor.getN(); beam_id++) {
                                    if (measurement_ref(beam_id, 0) <= angle_min && measurement_ref(beam_id, 0) >= angle_max) {
                                        valid_ids.push_back(beam_id);
                                    }
                                }
                            }
                            else {
                                for (int beam_id = 0; beam_id < sensor.getN(); beam_id++) {
                                    if (measurement_ref(beam_id, 0) >= angle_min && measurement_ref(beam_id, 0) <= angle_max) {
                                        valid_ids.push_back(beam_id);
                                    }
                                }
                            }
                            
                            // Update estimated distance
                            for (vector<int>::iterator beam_it = valid_ids.begin(); beam_it != valid_ids.end(); beam_it++) {
                                if (pixel_distance < map_range) {
                                    (*it).getMeasurementEstimate()((*beam_it), 1) = Map::map2world((int)pixel_distance);
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}


// +++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
// +++++++++++++++++++++++++++++++++++++++++++ Scan Matching +++++++++++++++++++++++++++++++++++++++++++++++++
// +++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

// Helper function to transform measurements from polar to cartesian coordinates
Eigen::MatrixX2f polar2cart(const Eigen::Vector3f& pose, const Eigen::MatrixX2f& measurements_polar){
    
    // Instantiate container for cartesian measurements
    Eigen::MatrixX2f measurements_cartesian = Eigen::MatrixX2f::Zero(measurements_polar.rows(), measurements_polar.cols());
    
    // Rotation matrix for local to global coordinates
    Eigen::Matrix2f R;
    R(0, 0) = cos((float)pose(2));
    R(1, 0) = sin((float)pose(2));
    R(0, 1) = -sin((float)pose(2));
    R(1, 1) = cos((float)pose(2));
    
    // Iterate over all measurements
    for (int beam_id = 0; beam_id < measurements_polar.rows(); beam_id++){
        
        // Compute x and y coordinates of measurement relative to robot
        measurements_cartesian(beam_id, 0) = measurements_polar(beam_id, 1) * cos(measurements_polar(beam_id, 0));
        measurements_cartesian(beam_id, 1) = measurements_polar(beam_id, 1) * sin(measurements_polar(beam_id, 0));
    }
    
    // Get x and y coordinates of current pose
    Eigen::Vector2f pose2d;
    pose2d(0) = (float)pose(0);
    pose2d(1) = (float)pose(1);
    
    // Rotate and translate measurements to obtain global coordinates
    measurements_cartesian = (R * measurements_cartesian.transpose()).transpose();
    measurements_cartesian.rowwise() += pose2d.block<2,1>(0, 0).transpose();
    
    return measurements_cartesian;
}


// Perform scan matching to estimate pose correction based on real and estimated measurements
void RBPF::scan_matching(const Eigen::Vector3f &pose, Sensor& sensor){
    
    // Transform measurements to cartesian coordinates
    Eigen::MatrixX2f measurements_cartesian = polar2cart(pose, sensor.getMeasurements());
    
    // Iterate over all particles
    for (list<Particle>::iterator it = this->particles.begin(); it != this->particles.end(); it++) {
    
        // Transform measurements to cartesian coordinates
        Eigen::MatrixX2f measurement_estimate_cartesian = polar2cart((*it).getPose(), (*it).getMeasurementEstimate());
        
        // Estimated pose correction using ICP (Iterative Closest Point) matching
        Eigen::Vector3f pose_dif = this->scan_matcher.ICP(measurement_estimate_cartesian, measurements_cartesian);
        
        // Update the particle's pose using the estimated pose correction
        (*it).getPose() += pose_dif;

    }
}


// +++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
// ++++++++++++++++++++++++++++++++++++++++++++++ Weighting ++++++++++++++++++++++++++++++++++++++++++++++++++
// +++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

void RBPF::weight(Sensor &sensor){
    
    // Get reference to current measurements
    Eigen::MatrixX2f measurement_ref = sensor.getMeasurements();
    
    // Instantiate container for weights
    vector<float> weights;
    
    // Iterate over all particles
    for (list<Particle>::iterator it = this->particles.begin(); it != this->particles.end(); it++) {
        
        // Get reference to estimated measurements
        Eigen::MatrixX2f measurement_estimate = (*it).getMeasurementEstimate();
        
        // Compute average likelihood of measurements
        float p = 0;
        for (int beam_id = 0; beam_id < measurement_ref.rows(); beam_id++){
            
            // Get difference between real and estimated measurement
            float scan_dif = abs(measurement_ref(beam_id, 1) - measurement_estimate(beam_id, 1));
            
            // Compute and accumulate likelihood of measurement based on gaussian measurement model
            p += exp(-0.5*pow((scan_dif/sensor.getQ()(1)), 2)) / (sqrt(2*PI) * sensor.getQ()(1));
        }
        
        // Compute average likelihood of measurements for currently inspected particle
        p /= this->getN();
        
        // Append weight to weight vector and assign to particle
        weights.push_back(p);
        (*it).getWeight() = p;
    }
    
    // Accumulate weights of all particles
    float sum_of_weights = std::accumulate(weights.begin(), weights.end(), 0.0);
    
    // If sum of weights close to 0, assign uniform weights to all particles
    if (sum_of_weights < 1e-3){
        cout << "Sum of Weights = 0" << endl;
        for (list<Particle>::iterator it = this->particles.begin(); it != this->particles.end(); it++) {
            (*it).getWeight() = 1.0 / this->getN();
        }
    }
    // Normalize weights to a sum of 1
    else {
        for (list<Particle>::iterator it = this->particles.begin(); it != this->particles.end(); it++) {
            (*it).getWeight() /= sum_of_weights;
        }
    }
}


// +++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
// +++++++++++++++++++++++++++++++++++++++++++++ Resampling ++++++++++++++++++++++++++++++++++++++++++++++++++
// +++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

void RBPF::resample(){
    
    // Create standard normal distribution for sampling
    default_random_engine generator;
    uniform_real_distribution<float> distribution(0.0, 1.0);
    
    // Instantiate containers for cumulative sum of weights and particle poses
    vector<float> cum_sum;
    vector<Eigen::Array3f> poses;
    
    // Initialize sum of weights to 0
    float sum = 0;
    
    // Iterate over all particles, accumulate weights and append poses
    for (list<Particle>::iterator it = this->particles.begin(); it != this->particles.end(); it++) {
        sum += (float)(*it).getWeight();
        cum_sum.push_back(sum);
        poses.push_back((*it).getPose());
    }
    
    // +++++++++++++++++++++++++++++++ Perform systematic resampling +++++++++++++++++++++++++++++++++++++++++
    
    // Sample random number in range [0, 1/N]
    float r = distribution(generator) / this->n_particles;
    
    // Instantiate container
    vector<int> particle_ids;
    
    // Iterate over all particles
    for (list<Particle>::iterator it = this->particles.begin(); it != this->particles.end(); it++) {
        
        // Get reference threshold for particle sampling
        float ref_sum = r + ((float)distance(this->particles.begin(), it) / this->n_particles);
        
        // Select index of first particle for which cumulative sum exceeds reference threshold
        for (vector<float>::iterator sit = cum_sum.begin(); sit != cum_sum.end(); sit++) {
            if ((*sit) >= ref_sum) {
                int particle_id = (int) distance(cum_sum.begin(), sit);
                particle_ids.push_back(particle_id);
                break;
            }
        }
    }
    
    // Resample particles based on selected IDs
    for (list<Particle>::iterator it = this->particles.begin(); it != this->particles.end(); it++) {
        
        // Get ID of sampled particle and assign corresponding pose
        int particle_id = particle_ids[(int)distance(this->particles.begin(), it)];
        (*it).getPose() = poses[particle_id];
        
        // Reset weight of particle to 1/N
        (*it).getWeight() = 1.0 / this->n_particles;
    }
}


// +++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
// +++++++++++++++++++++++++++++++++++++++++++++ Mapping +++++++++++++++++++++++++++++++++++++++++++++++++++++
// +++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

// Inverse sensor model to estimate grid cell update from real measurements
int RBPF::inverse_sensor_model(const int &x_px, const int &y_px, Eigen::Vector3f& map_pose, Sensor &sensor){
    
    // Occupancy update variable
    int occupancy_update;
    
    // Inverse model parameters
    const float alpha = 1.0; // thickness of object in map coordinates
    const float beta = 0.1; // width of a laser beam in map coordinates
    const float max_range = Map::world2map(sensor.getRange());
    
    // Get mass center of currently inspected pixel
    const float xc_px = x_px + 0.5;
    const float yc_px = y_px + 0.5;
    
    // Get mass center of robot's center pixel
    const float xc_r = (float)map_pose(0) + 0.5;
    const float yc_r = (float)map_pose(1) + 0.5;
    const float heading_r = (float)map_pose(2);
    
    // Distance from robot to mass center of currently inspected pixel
    const float pixel_distance = sqrt(pow(xc_px - xc_r, 2) + pow(yc_px - yc_r, 2));
    
    // Angle of currently inspected pixel relative to robot's heading
    float pixel_angle = atan2(yc_px - yc_r, xc_px - xc_r) - heading_r;
    // Keep angle within range [-pi, pi)
    if (pixel_angle < -PI) {
        pixel_angle = fmod(pixel_angle-PI, 2*PI) + PI; }
    else {
        pixel_angle = fmod(pixel_angle+PI, 2*PI) - PI; }

    // Check if inspected pixel lies in sensor's FoV
    if (pixel_angle < -((float)sensor.getFoV()/360.0*PI) || pixel_angle > ((float)sensor.getFoV()/360*PI)) {
        return occupancy_update = 0; }
    else {
    
        // Select laser beam with the minimum angle difference to the calculated pixel angle
        vector<float> angle_dif;
        for(int beam_id = 0; beam_id < sensor.getN(); beam_id++) {
            angle_dif.push_back(abs(pixel_angle - sensor.getMeasurements()(beam_id, 0)));
        }
        
        // Index of selected measurement
        int beam_id = (int) distance(angle_dif.begin(), min_element(angle_dif.begin(), angle_dif.end()));
    
        // Get corresponding range and relative angle of the selected measurement
        int detected_range = Map::world2map(sensor.getMeasurements()(beam_id, 1));
        const float max_detection_range = detected_range + alpha/2.0;
        float beam_angle = sensor.getMeasurements()(beam_id, 0);
        
        // Get occupancy value update
        // If angle difference larger than beam width and pixel distance greater than measured range, no
        // update
        if ((abs(pixel_angle-beam_angle) > (beta/2)) || pixel_distance >= min(max_range, max_detection_range)) {
            occupancy_update = 0; }
        // If detected range within sensor range and difference between detected range and pixel distance smaller than object width, occupied pixel detected
        else if (detected_range < max_range && (abs(pixel_distance-detected_range) < alpha/2.0)) {
            occupancy_update = -Map::getValueStep(); }
        // If the distance to the currently inspected pixel is shorter than the detected range, unoccupied pixel detected
        else if (pixel_distance <= detected_range) {
            occupancy_update = Map::getValueStep(); }
        else {
            cout << "Grid occupancy failed" << endl;
            exit(1);
        }
    
        return occupancy_update;
    }
}


// Occupancy grid mapping
void RBPF::mapping(Sensor &sensor){
   
    // Transform the sensor's maximum range to map scale
    int map_range = Map::world2map(sensor.getRange());
    
    // Iterate over all particles
    for (list<Particle>::iterator it = this->particles.begin(); it != this->particles.end(); it++) {
        
        // Transform particle pose from world coordinates to map coordinates
        Eigen::Vector3f map_pose = Map::world2map((*it).getPose());
        
        // Get reference to the currently inspected particle's map data
        cv::Mat map_ref = (*it).getMap().getData();
        
        // Iterate over all vertical pixels within range of the sensor from robot's current position
        for (int y_px = ((int)map_pose(1) - map_range); y_px <= ((int)(map_pose(1) + map_range)); y_px++) {
            // check that index lies within map boundaries
            if (y_px >= 0 && y_px < (int)(Map::getHeight() / Map::getResolution())) {
                
                // Get a pointer to the beginning of the currently inspected row to access all relevant
                // horizontal pixels
                uchar* horizontal_pixel_ptr = map_ref.ptr<uchar>(y_px);
                
                // Iterate over all horizontal pixels within range of the sensor from robot's current position
                for (int x_px = ((int)map_pose(0) - map_range); x_px <= ((int)(map_pose(0) + map_range)); x_px++) {
                    // Check that index lies within map boundaries
                    if (x_px >= 0 && x_px < (int)(Map::getHeight() / Map::getResolution())) {
                        
                        // Get occupancy information from map for currently inspected pixel
                        float occupancy_update = this->inverse_sensor_model(x_px, y_px, map_pose, sensor);
                        
                        // Update map with new occupancy information
                        // Keep values within range of specified values in case maximum or minimum is reached
                        if (horizontal_pixel_ptr[x_px] >= (Map::getValueMax() - Map::getValueStep())) {
                            horizontal_pixel_ptr[x_px] = Map::getValueMax(); }
                        else if (horizontal_pixel_ptr[x_px] <= Map::getValueStep()) {
                            horizontal_pixel_ptr[x_px] = Map::getValueMin(); }
                        else { horizontal_pixel_ptr[x_px] += occupancy_update; }
                    }
                }
            }
        }
    }
}


// +++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
// +++++++++++++++++++++++++++++++++++++++++++++ Get Estimates +++++++++++++++++++++++++++++++++++++++++++++++
// +++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

// Get current best map estimate
Map& RBPF::getMap(){
    
    // Instantiate pointer and set initial best weight to 0
    Particle* best_particle_ptr = 0;
    float best_particle_weight = 0.0;
    
    // Iterate over all particles
    for (list<Particle>::iterator it = this->particles.begin(); it != this->particles.end(); it++) {
        
        // Select currently inspected particle if weight higher than previously highest weight
        if ((*it).getWeight() > best_particle_weight) {
            best_particle_weight = (*it).getWeight();
            best_particle_ptr = &(*it);
        }
    }
    
    // Return map of particle with highest weight
    return best_particle_ptr->getMap();
    
}
