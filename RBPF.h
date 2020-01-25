//
//  RBPF.h
//  FastSLAM
//
//  Created by Mats Steinweg on 20.08.19.
//  Copyright © 2019 Mats Steinweg. All rights reserved.
//

#ifndef RBPF_h
#define RBPF_h

#include <list>
#include <Eigen/Dense>

#include "Particle.h"
#include "Sensor.h"
#include "ScanMatcher.h"

class Robot;

using namespace std;

class RBPF {
    
    public:
    // constructor and destructor
    RBPF();
    RBPF(int n_particles);
    ~RBPF(){};
    
    // summary of RBPF
    void summary();
    
    // functionality of the particle filter
    void predict(const float& v, const float& omega, const float& current_timestamp);
    void sweep_estimate(Sensor& sensor, const Eigen::Array3f& pose);
    void mapping(Sensor& sensor, const Eigen::Array3f& pose);
    void run(Robot &robot);
    int inverse_sensor_model(const int& x, const int& y, Eigen::Array3f& image_pose, Sensor& sensor);
    void scan_matching(const Eigen::Array3f &pose, Sensor& sensor);
    void weight(Sensor& sensor);
    void resample();
    
    // getter functions
    const cv::Mat& getMap();
    list<Particle>& getParticles(){ return this->particles; };
    const int getN(){ return this->n_particles; };
    
    // setter functions
    void setScanMatcher(ScanMatcher scan_matcher){ this->scan_matcher = scan_matcher; };
    
    private:
    float last_timestamp;
    list<Particle> particles;
    int n_particles;
    Eigen::Array3f R;
    ScanMatcher scan_matcher;
    
};


#endif /* RBPF_h */