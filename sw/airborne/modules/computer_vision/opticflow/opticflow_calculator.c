/*
 * Copyright (C) 2014 Hann Woei Ho
 *               2015 Freek van Tienen <freek.v.tienen@gmail.com>
 *               2016 Kimberly McGuire <k.n.mcguire@tudelft.nl
 *
 * This file is part of Paparazzi.
 *
 * Paparazzi is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * Paparazzi is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Paparazzi; see the file COPYING.  If not, see
 * <http://www.gnu.org/licenses/>.
 */

/**
 * @file modules/computer_vision/opticflow/opticflow_calculator.c
 * @brief Estimate velocity from optic flow.
 *
 * Using images from a vertical camera and IMU sensor data.
 */

#include "std.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// Own Header
#include "opticflow_calculator.h"

// Computer Vision
#include "lib/vision/image.h"
#include "lib/vision/lucas_kanade.h"
#include "lib/vision/fast_rosten.h"
#include "lib/vision/edge_flow.h"
#include "size_divergence.h"
#include "linear_flow_fit.h"

// Kalman filter
#include "lib/filters/kalman_filter_vision.h"
#include "subsystems/imu.h"


// whether to show the flow and corners:
#define OPTICFLOW_SHOW_FLOW 0
#define OPTICFLOW_SHOW_CORNERS 0

// What methods are run to determine divergence, lateral flow, etc.
// SIZE_DIV looks at line sizes and only calculates divergence
#define SIZE_DIV 1
// LINEAR_FIT makes a linear optical flow field fit and extracts a lot of information:
// relative velocities in x, y, z (divergence / time to contact), the slope of the surface, and the surface roughness.
#define LINEAR_FIT 1

// Camera parameters (defaults are from an ARDrone 2)
#ifndef OPTICFLOW_FOV_W
#define OPTICFLOW_FOV_W 0.89360857702
#endif
PRINT_CONFIG_VAR(OPTICFLOW_FOV_W)

#ifndef OPTICFLOW_FOV_H
#define OPTICFLOW_FOV_H 0.67020643276
#endif
PRINT_CONFIG_VAR(OPTICFLOW_FOV_H)

#ifndef OPTICFLOW_FX
#define OPTICFLOW_FX 343.1211
#endif
PRINT_CONFIG_VAR(OPTICFLOW_FX)

#ifndef OPTICFLOW_FY
#define OPTICFLOW_FY 348.5053
#endif
PRINT_CONFIG_VAR(OPTICFLOW_FY)

/* Set the default values */
#ifndef OPTICFLOW_MAX_TRACK_CORNERS
#define OPTICFLOW_MAX_TRACK_CORNERS 25
#endif
PRINT_CONFIG_VAR(OPTICFLOW_MAX_TRACK_CORNERS)

#ifndef OPTICFLOW_WINDOW_SIZE
#define OPTICFLOW_WINDOW_SIZE 10
#endif
PRINT_CONFIG_VAR(OPTICFLOW_WINDOW_SIZE)

#ifndef OPTICFLOW_SEARCH_DISTANCE
#define OPTICFLOW_SEARCH_DISTANCE 20
#endif
PRINT_CONFIG_VAR(OPTICFLOW_SEARCH_DISTANCE)

#ifndef OPTICFLOW_SUBPIXEL_FACTOR
#define OPTICFLOW_SUBPIXEL_FACTOR 10
#endif
PRINT_CONFIG_VAR(OPTICFLOW_SUBPIXEL_FACTOR)

#ifndef OPTICFLOW_RESOLUTION_FACTOR
#define OPTICFLOW_RESOLUTION_FACTOR 100
#endif
PRINT_CONFIG_VAR(OPTICFLOW_RESOLUTION_FACTOR)

#ifndef OPTICFLOW_MAX_ITERATIONS
#define OPTICFLOW_MAX_ITERATIONS 10
#endif
PRINT_CONFIG_VAR(OPTICFLOW_MAX_ITERATIONS)

#ifndef OPTICFLOW_THRESHOLD_VEC
#define OPTICFLOW_THRESHOLD_VEC 2
#endif
PRINT_CONFIG_VAR(OPTICFLOW_THRESHOLD_VEC)

#ifndef OPTICFLOW_PYRAMID_LEVEL
#define OPTICFLOW_PYRAMID_LEVEL 0
#endif
PRINT_CONFIG_VAR(OPTICFLOW_PYRAMID_LEVEL)

#ifndef OPTICFLOW_FAST9_ADAPTIVE
#define OPTICFLOW_FAST9_ADAPTIVE TRUE
#endif
PRINT_CONFIG_VAR(OPTICFLOW_FAST9_ADAPTIVE)

#ifndef OPTICFLOW_FAST9_THRESHOLD
#define OPTICFLOW_FAST9_THRESHOLD 20
#endif
PRINT_CONFIG_VAR(OPTICFLOW_FAST9_THRESHOLD)

#ifndef OPTICFLOW_FAST9_MIN_DISTANCE
#define OPTICFLOW_FAST9_MIN_DISTANCE 10
#endif
PRINT_CONFIG_VAR(OPTICFLOW_FAST9_MIN_DISTANCE)

#ifndef OPTICFLOW_FAST9_PADDING
#define OPTICFLOW_FAST9_PADDING 20
#endif
PRINT_CONFIG_VAR(OPTICFLOW_FAST9_PADDING)

// thresholds FAST9 that are currently not set from the GCS:
#define FAST9_LOW_THRESHOLD 5
#define FAST9_HIGH_THRESHOLD 60

#ifndef OPTICFLOW_METHOD
#define OPTICFLOW_METHOD 0
#endif
PRINT_CONFIG_VAR(OPTICFLOW_METHOD)

#if OPTICFLOW_METHOD > 1
#error WARNING: Both Lukas Kanade and EdgeFlow are NOT selected
#endif

#ifndef OPTICFLOW_DEROTATION
#define OPTICFLOW_DEROTATION TRUE
#endif
PRINT_CONFIG_VAR(OPTICFLOW_DEROTATION)

#ifndef OPTICFLOW_DEROTATION_CORRECTION_FACTOR_X
#define OPTICFLOW_DEROTATION_CORRECTION_FACTOR_X 1.0
#endif
PRINT_CONFIG_VAR(OPTICFLOW_DEROTATION_CORRECTION_FACTOR_X)

#ifndef OPTICFLOW_DEROTATION_CORRECTION_FACTOR_Y
#define OPTICFLOW_DEROTATION_CORRECTION_FACTOR_Y 1.0
#endif
PRINT_CONFIG_VAR(OPTICFLOW_DEROTATION_CORRECTION_FACTOR_Y)

#ifndef OPTICFLOW_MEDIAN_FILTER
#define OPTICFLOW_MEDIAN_FILTER FALSE
#endif
PRINT_CONFIG_VAR(OPTICFLOW_MEDIAN_FILTER)

#ifndef OPTICFLOW_KALMAN_FILTER
#define OPTICFLOW_KALMAN_FILTER TRUE
#endif
PRINT_CONFIG_VAR(OPTICFLOW_KALMAN_FILTER)

#ifndef OPTICFLOW_KALMAN_FILTER_PROCESS_NOISE
#define OPTICFLOW_KALMAN_FILTER_PROCESS_NOISE 0.01
#endif
PRINT_CONFIG_VAR(OPTICFLOW_KALMAN_FILTER_PROCESS_NOISE)

//Include median filter
#include "filters/median_filter.h"
struct MedianFilterInt vel_x_filt, vel_y_filt;


/* Functions only used here */
static uint32_t timeval_diff(struct timeval *starttime, struct timeval *finishtime);
static int cmp_flow(const void *a, const void *b);



/**
 * Initialize the opticflow calculator
 * @param[out] *opticflow The new optical flow calculator
 */
void opticflow_calc_init(struct opticflow_t *opticflow)
{
  /* Set the default values */
  opticflow->method = OPTICFLOW_METHOD; //0 = LK_fast9, 1 = Edgeflow
  opticflow->window_size = OPTICFLOW_WINDOW_SIZE;
  opticflow->search_distance = OPTICFLOW_SEARCH_DISTANCE;
  opticflow->derotation = OPTICFLOW_DEROTATION; //0 = OFF, 1 = ON
  opticflow->derotation_correction_factor_x = OPTICFLOW_DEROTATION_CORRECTION_FACTOR_X;
  opticflow->derotation_correction_factor_y = OPTICFLOW_DEROTATION_CORRECTION_FACTOR_Y;

  opticflow->max_track_corners = OPTICFLOW_MAX_TRACK_CORNERS;
  opticflow->subpixel_factor = OPTICFLOW_SUBPIXEL_FACTOR;
  opticflow->resolution_factor = OPTICFLOW_RESOLUTION_FACTOR;
  opticflow->max_iterations = OPTICFLOW_MAX_ITERATIONS;
  opticflow->threshold_vec = OPTICFLOW_THRESHOLD_VEC;
  opticflow->pyramid_level = OPTICFLOW_PYRAMID_LEVEL;
  opticflow->median_filter = OPTICFLOW_MEDIAN_FILTER;
  opticflow->kalman_filter = OPTICFLOW_KALMAN_FILTER;
  opticflow->kalman_filter_process_noise = OPTICFLOW_KALMAN_FILTER_PROCESS_NOISE;

  opticflow->fast9_adaptive = OPTICFLOW_FAST9_ADAPTIVE;
  opticflow->fast9_threshold = OPTICFLOW_FAST9_THRESHOLD;
  opticflow->fast9_min_distance = OPTICFLOW_FAST9_MIN_DISTANCE;
  opticflow->fast9_padding = OPTICFLOW_FAST9_PADDING;
  opticflow->fast9_rsize = 512;
  opticflow->fast9_ret_corners = malloc(sizeof(struct point_t) * opticflow->fast9_rsize);
}
/**
 * Run the optical flow with fast9 and lukaskanade on a new image frame
 * @param[in] *opticflow The opticalflow structure that keeps track of previous images
 * @param[in] *state The state of the drone
 * @param[in] *img The image frame to calculate the optical flow from
 * @param[out] *result The optical flow result
 */
void calc_fast9_lukas_kanade(struct opticflow_t *opticflow, struct opticflow_state_t *state, struct image_t *img,
                             struct opticflow_result_t *result)
{
  if (opticflow->just_switched_method) {
	// Create the image buffers
	image_create(&opticflow->img_gray, img->w, img->h, IMAGE_GRAYSCALE);
	image_create(&opticflow->prev_img_gray, img->w, img->h, IMAGE_GRAYSCALE);

	// Set the previous values
	opticflow->got_first_img = false;
	FLOAT_RATES_ZERO(opticflow->prev_rates);

	// Init median filters with zeros
	init_median_filter(&vel_x_filt);
	init_median_filter(&vel_y_filt);
  }

  // variables for size_divergence:
  float size_divergence; int n_samples;

  // variables for linear flow fit:
  float error_threshold;
  int n_iterations_RANSAC, n_samples_RANSAC, success_fit;
  struct linear_flow_fit_info fit_info;

  // Update FPS for information
  result->fps = 1 / (timeval_diff(&opticflow->prev_timestamp, &img->ts) / 1000.);
  opticflow->prev_timestamp = img->ts;

  // Convert image to grayscale
  image_to_grayscale(img, &opticflow->img_gray);

  // Copy to previous image if not set
  if (!opticflow->got_first_img) {
    image_copy(&opticflow->img_gray, &opticflow->prev_img_gray);
    opticflow->got_first_img = true;
  }

  // *************************************************************************************
  // Corner detection
  // *************************************************************************************

  // FAST corner detection
  // TODO: There is something wrong with fast9_detect destabilizing FPS. This problem is reduced with putting min_distance
  // to 0 (see defines), however a more permanent solution should be considered
  fast9_detect(img, opticflow->fast9_threshold, opticflow->fast9_min_distance,
               opticflow->fast9_padding, opticflow->fast9_padding, &result->corner_cnt,
               &opticflow->fast9_rsize,
               opticflow->fast9_ret_corners);

  // Adaptive threshold
  if (opticflow->fast9_adaptive) {
    // Decrease and increase the threshold based on previous values
    if (result->corner_cnt < 40
        && opticflow->fast9_threshold > FAST9_LOW_THRESHOLD) { // TODO: Replace 40 with OPTICFLOW_MAX_TRACK_CORNERS / 2
      opticflow->fast9_threshold--;
    } else if (result->corner_cnt > OPTICFLOW_MAX_TRACK_CORNERS * 2 && opticflow->fast9_threshold < FAST9_HIGH_THRESHOLD) {
      opticflow->fast9_threshold++;
    }
  }

#if OPTICFLOW_SHOW_CORNERS
  image_show_points(img, opticflow->fast9_ret_corners, result->corner_cnt);
#endif

  // Check if we found some corners to track
  if (result->corner_cnt < 1) {
    image_copy(&opticflow->img_gray, &opticflow->prev_img_gray);
    return;
  }

  // *************************************************************************************
  // Corner Tracking
  // *************************************************************************************

  // Execute a Lucas Kanade optical flow
  result->tracked_cnt = result->corner_cnt;
  struct flow_t *vectors = opticFlowLK(&opticflow->img_gray, &opticflow->prev_img_gray, opticflow->fast9_ret_corners,
                                       &result->tracked_cnt,
                                       opticflow->window_size / 2, opticflow->subpixel_factor, opticflow->max_iterations,
                                       opticflow->threshold_vec, opticflow->max_track_corners, opticflow->pyramid_level);

#if OPTICFLOW_SHOW_FLOW
  printf("show: n tracked = %d\n", result->tracked_cnt);
  image_show_flow(img, vectors, result->tracked_cnt, opticflow->subpixel_factor);
#endif

  // Estimate size divergence:
  if (SIZE_DIV) {
    n_samples = 100;
    size_divergence = get_size_divergence(vectors, result->tracked_cnt, n_samples);
    result->div_size = size_divergence;
  } else {
    result->div_size = 0.0f;
  }
  if (LINEAR_FIT) {
    // Linear flow fit (normally derotation should be performed before):
    error_threshold = 10.0f;
    n_iterations_RANSAC = 20;
    n_samples_RANSAC = 5;
    success_fit = analyze_linear_flow_field(vectors, result->tracked_cnt, error_threshold, n_iterations_RANSAC,
                                            n_samples_RANSAC, img->w, img->h, &fit_info);

    if (!success_fit) {
      fit_info.divergence = 0.0f;
      fit_info.surface_roughness = 0.0f;
    }

    result->divergence = fit_info.divergence;
    result->surface_roughness = fit_info.surface_roughness;
  } else {
    result->divergence = 0.0f;
    result->surface_roughness = 0.0f;
  }


  // Get the median flow
  qsort(vectors, result->tracked_cnt, sizeof(struct flow_t), cmp_flow);
  if (result->tracked_cnt == 0) {
    // We got no flow
    result->flow_x = 0;
    result->flow_y = 0;
  } else if (result->tracked_cnt > 3) {
    // Take the average of the 3 median points
    result->flow_x = vectors[result->tracked_cnt / 2 - 1].flow_x;
    result->flow_y = vectors[result->tracked_cnt / 2 - 1].flow_y;
    result->flow_x += vectors[result->tracked_cnt / 2].flow_x;
    result->flow_y += vectors[result->tracked_cnt / 2].flow_y;
    result->flow_x += vectors[result->tracked_cnt / 2 + 1].flow_x;
    result->flow_y += vectors[result->tracked_cnt / 2 + 1].flow_y;
    result->flow_x /= 3;
    result->flow_y /= 3;
  } else {
    // Take the median point
    result->flow_x = vectors[result->tracked_cnt / 2].flow_x;
    result->flow_y = vectors[result->tracked_cnt / 2].flow_y;
  }

  // Flow Derotation
  float diff_flow_x = 0;
  float diff_flow_y = 0;

  /*// Flow Derotation TODO:
  float diff_flow_x = (state->phi - opticflow->prev_phi) * img->w / OPTICFLOW_FOV_W;
  float diff_flow_y = (state->theta - opticflow->prev_theta) * img->h / OPTICFLOW_FOV_H;*/

  if (opticflow->derotation && result->tracked_cnt > 5) {
    diff_flow_x = (state->rates.p)  / result->fps * img->w /
                  OPTICFLOW_FOV_W;// * img->w / OPTICFLOW_FOV_W;
    diff_flow_y = (state->rates.q) / result->fps * img->h /
                  OPTICFLOW_FOV_H;// * img->h / OPTICFLOW_FOV_H;
  }

  result->flow_der_x = result->flow_x - diff_flow_x * opticflow->subpixel_factor *
                       opticflow->derotation_correction_factor_x;
  result->flow_der_y = result->flow_y - diff_flow_y * opticflow->subpixel_factor *
                       opticflow->derotation_correction_factor_y;
  opticflow->prev_rates = state->rates;

  // Velocity calculation
  // Right now this formula is under assumption that the flow only exist in the center axis of the camera.
  // TODO Calculate the velocity more sophisticated, taking into account the drone's angle and the slope of the ground plane.
  float vel_x = result->flow_der_x * result->fps * state->agl / opticflow->subpixel_factor  / OPTICFLOW_FX;
  float vel_y = result->flow_der_y * result->fps * state->agl / opticflow->subpixel_factor  / OPTICFLOW_FY;

  //Apply a  median filter to the velocity if wanted
  if (opticflow->median_filter == true) {
    result->vel_x = (float)update_median_filter(&vel_x_filt, (int32_t)(vel_x * 1000)) / 1000;
    result->vel_y = (float)update_median_filter(&vel_y_filt, (int32_t)(vel_y * 1000)) / 1000;
  } else {
    result->vel_x = vel_x;
    result->vel_y = vel_y;
  }
  // Velocity calculation: uncomment if focal length of the camera is not known or incorrect.
  //  result->vel_x =  - result->flow_der_x * result->fps * state->agl / opticflow->subpixel_factor * OPTICFLOW_FOV_W / img->w
  //  result->vel_y =  result->flow_der_y * result->fps * state->agl / opticflow->subpixel_factor * OPTICFLOW_FOV_H / img->h


  // Determine quality of noise measurement for state filter
  //TODO develop a noise model based on groundtruth

  float noise_measurement_temp = (1 - ((float)result->tracked_cnt / ((float)opticflow->max_track_corners * 1.25)));
  result->noise_measurement = noise_measurement_temp;

  // *************************************************************************************
  // Next Loop Preparation
  // *************************************************************************************
  free(vectors);
  image_switch(&opticflow->img_gray, &opticflow->prev_img_gray);
}

/**
 * Run the optical flow with EDGEFLOW on a new image frame
 * @param[in] *opticflow The opticalflow structure that keeps track of previous images
 * @param[in] *state The state of the drone
 * @param[in] *img The image frame to calculate the optical flow from
 * @param[out] *result The optical flow result
 */
void calc_edgeflow_tot(struct opticflow_t *opticflow, struct opticflow_state_t *state, struct image_t *img,
                       struct opticflow_result_t *result)
{
  // Define Static Variables
  static struct edge_hist_t edge_hist[MAX_HORIZON];
  static uint8_t current_frame_nr = 0;
  struct edge_flow_t edgeflow;
  static uint8_t previous_frame_offset[2] = {1, 1};

  // Define Normal variables
  struct edgeflow_displacement_t displacement;
  displacement.x = malloc(sizeof(int32_t) * img->w);
  displacement.y = malloc(sizeof(int32_t) * img->h);

  // If the methods just switched to this one, reintialize the
  // array of edge_hist structure.
  if (opticflow->just_switched_method == 1) {
    int i;
    for (i = 0; i < MAX_HORIZON; i++) {
      edge_hist[i].x = malloc(sizeof(int32_t) * img->w);
      edge_hist[i].y = malloc(sizeof(int32_t) * img->h);
      FLOAT_RATES_ZERO(edge_hist[i].rates);
    }
  }

  uint16_t disp_range;
  if (opticflow->search_distance < DISP_RANGE_MAX) {
    disp_range = opticflow->search_distance;
  } else {
    disp_range = DISP_RANGE_MAX;
  }

  uint16_t window_size;

  if (opticflow->window_size < MAX_WINDOW_SIZE) {
    window_size = opticflow->window_size;
  } else {
    window_size = MAX_WINDOW_SIZE;
  }

  uint16_t RES = opticflow->resolution_factor;

  //......................Calculating EdgeFlow..................... //

  // Calculate current frame's edge histogram
  int32_t *edge_hist_x = edge_hist[current_frame_nr].x;
  int32_t *edge_hist_y = edge_hist[current_frame_nr].y;
  calculate_edge_histogram(img, edge_hist_x, 'x', 0);
  calculate_edge_histogram(img, edge_hist_y, 'y', 0);


  // Copy frame time and angles of image to calculated edge histogram
  edge_hist[current_frame_nr].frame_time = img->ts;
  edge_hist[current_frame_nr].rates = state->rates;

  // Calculate which previous edge_hist to compare with the current
  uint8_t previous_frame_nr[2];
  calc_previous_frame_nr(result, opticflow, current_frame_nr, previous_frame_offset, previous_frame_nr);

  //Select edge histogram from the previous frame nr
  int32_t *prev_edge_histogram_x = edge_hist[previous_frame_nr[0]].x;
  int32_t *prev_edge_histogram_y = edge_hist[previous_frame_nr[1]].y;

  //Calculate the corresponding derotation of the two frames
  int16_t der_shift_x = 0;
  int16_t der_shift_y = 0;

  if (opticflow->derotation) {
    der_shift_x = (int16_t)(edge_hist[current_frame_nr].rates.p  /
                            result->fps *
                            (float)img->w / (OPTICFLOW_FOV_W) * opticflow->derotation_correction_factor_x);
    der_shift_y = (int16_t)(edge_hist[current_frame_nr].rates.q /
                            result->fps *
                            (float)img->h / (OPTICFLOW_FOV_H) * opticflow->derotation_correction_factor_y);
  }

  // Estimate pixel wise displacement of the edge histograms for x and y direction
  calculate_edge_displacement(edge_hist_x, prev_edge_histogram_x,
                              displacement.x, img->w,
                              window_size, disp_range,  der_shift_x);
  calculate_edge_displacement(edge_hist_y, prev_edge_histogram_y,
                              displacement.y, img->h,
                              window_size, disp_range, der_shift_y);

  // Fit a line on the pixel displacement to estimate
  // the global pixel flow and divergence (RES is resolution)
  line_fit(displacement.x, &edgeflow.div_x,
           &edgeflow.flow_x, img->w,
           window_size + disp_range, RES);
  line_fit(displacement.y, &edgeflow.div_y,
           &edgeflow.flow_y, img->h,
           window_size + disp_range, RES);

  /* Save Resulting flow in results
   * Warning: The flow detected here is different in sign
   * and size, therefore this will be divided with
   * the same subpixel factor and -1 to make it on par with
   * the LK algorithm of t opticalflow_calculator.c
   * */
  edgeflow.flow_x = -1 * edgeflow.flow_x;
  edgeflow.flow_y = -1 * edgeflow.flow_y;

  edgeflow.flow_x = (int16_t)edgeflow.flow_x / previous_frame_offset[0];
  edgeflow.flow_y = (int16_t)edgeflow.flow_y / previous_frame_offset[1];

  result->flow_x = (int16_t)edgeflow.flow_x / RES;
  result->flow_y = (int16_t)edgeflow.flow_y / RES;

  //Fill up the results optic flow to be on par with LK_fast9
  result->flow_der_x =  result->flow_x;
  result->flow_der_y =  result->flow_y;
  result->corner_cnt = getAmountPeaks(edge_hist_x, 500 , img->w);
  result->tracked_cnt = getAmountPeaks(edge_hist_x, 500 , img->w);
  result->divergence = (float)edgeflow.div_x / RES;
  result->div_size = 0.0f;
  result->noise_measurement = 0.0f;
  result->surface_roughness = 0.0f;

  //......................Calculating VELOCITY ..................... //

  /*Estimate fps per direction
   * This is the fps with adaptive horizon for subpixel flow, which is not similar
   * to the loop speed of the algorithm. The faster the quadcopter flies
   * the higher it becomes
  */
  float fps_x = 0;
  float fps_y = 0;
  float time_diff_x = (float)(timeval_diff(&edge_hist[previous_frame_nr[0]].frame_time, &img->ts)) / 1000.;
  float time_diff_y = (float)(timeval_diff(&edge_hist[previous_frame_nr[1]].frame_time, &img->ts)) / 1000.;
  fps_x = 1 / (time_diff_x);
  fps_y = 1 / (time_diff_y);

  result->fps = fps_x;

  // Calculate velocity
  float vel_x = edgeflow.flow_x * fps_x * state->agl * OPTICFLOW_FOV_W / (img->w * RES);
  float vel_y = edgeflow.flow_y * fps_y * state->agl * OPTICFLOW_FOV_H / (img->h * RES);

  //Apply a  median filter to the velocity if wanted
  if (opticflow->median_filter == true) {
    result->vel_x = (float)update_median_filter(&vel_x_filt, (int32_t)(vel_x * 1000)) / 1000;
    result->vel_y = (float)update_median_filter(&vel_y_filt, (int32_t)(vel_y * 1000)) / 1000;
  } else {
    result->vel_x = vel_x;
    result->vel_y = vel_y;
  }

  result->noise_measurement = 0.2;



#if OPTICFLOW_SHOW_FLOW
  draw_edgeflow_img(img, edgeflow, prev_edge_histogram_x, edge_hist_x);
#endif
  // Increment and wrap current time frame
  current_frame_nr = (current_frame_nr + 1) % MAX_HORIZON;

  // Free malloc'd variables
  free(displacement.x);
  free(displacement.y);
}


/**
 * Run the optical flow on a new image frame
 * @param[in] *opticflow The opticalflow structure that keeps track of previous images
 * @param[in] *state The state of the drone
 * @param[in] *img The image frame to calculate the optical flow from
 * @param[out] *result The optical flow result
 */
void opticflow_calc_frame(struct opticflow_t *opticflow, struct opticflow_state_t *state, struct image_t *img,
                          struct opticflow_result_t *result)
{

  // A switch counter that checks in the loop if the current method is similar,
  // to the previous (for reinitializing structs)
  static int8_t switch_counter = -1;
  if (switch_counter != opticflow->method) {
    opticflow->just_switched_method = true;
    switch_counter = opticflow->method;
  } else {
    opticflow->just_switched_method = false;
  }

  // Switch between methods (0 = fast9/lukas-kanade, 1 = EdgeFlow)
  if (opticflow->method == 0) {
    calc_fast9_lukas_kanade(opticflow, state, img, result);
  } else if (opticflow->method == 1) {
    calc_edgeflow_tot(opticflow, state, img, result);
  }

  /* Rotate velocities from camera frame coordinates to body coordinates for control
  * IMPORTANT!!! This frame to body orientation should be the case for the Parrot
  * ARdrone and Bebop, however this can be different for other quadcopters
  * ALWAYS double check!
  */
  result->vel_body_x = result->vel_y;
  result->vel_body_y = - result->vel_x;

  // KALMAN filter on velocity
  float measurement_noise[2] = {result->noise_measurement, 1.0f};
  static bool reinitialize_kalman = true;

  static uint8_t wait_filter_counter =
    0; // When starting up the opticalflow module, or switching between methods, wait for a bit to prevent bias


  if (opticflow->kalman_filter == true) {
    if (opticflow->just_switched_method == true) {
      wait_filter_counter = 0;
      reinitialize_kalman = true;
    }

    if (wait_filter_counter > 50) {

      // Get accelerometer values rotated to body axis
      // TODO: use acceleration from the state ?
      struct FloatVect3 accel_imu_f;
      ACCELS_FLOAT_OF_BFP(accel_imu_f, state->accel_imu_meas);
      struct FloatVect3 accel_meas_body;
      float_quat_vmult(&accel_meas_body, &state->imu_to_body_quat, &accel_imu_f);

      float acceleration_measurement[2];
      acceleration_measurement[0] = accel_meas_body.x;
      acceleration_measurement[1] = accel_meas_body.y;

      kalman_filter_opticflow_velocity(&result->vel_body_x, &result->vel_body_y, acceleration_measurement, result->fps,
                                       measurement_noise, opticflow->kalman_filter_process_noise, reinitialize_kalman);
      if (reinitialize_kalman) {
        reinitialize_kalman = false;
      }

    } else {
      wait_filter_counter++;
    }
  } else {
    reinitialize_kalman = true;
  }

}

/**
 * Filter the velocity with a simple linear kalman filter, together with the accelerometers
 * @param[in] *velocity_x  Velocity in x direction of body fixed coordinates
 * @param[in] *velocity_y  Belocity in y direction of body fixed coordinates
 * @param[in] *acceleration_measurement  Measurements of the accelerometers
 * @param[in] fps  Frames per second
 * @param[in] *measurement_noise  Expected variance of the noise of the measurements
 * @param[in] *measurement_noise  Expected variance of the noise of the model prediction
 * @param[in] reinitialize_kalman  Boolean to reinitialize the kalman filter
 */
void kalman_filter_opticflow_velocity(float *velocity_x, float *velocity_y, float *acceleration_measurement, float fps,
                                      float *measurement_noise, float kalman_process_noise, bool reinitialize_kalman)
{
  // Initialize variables
  static float covariance_x[4], covariance_y[4], state_estimate_x[2], state_estimate_y[2];
  float measurements_x[2], measurements_y[2];

  if (reinitialize_kalman) {
    state_estimate_x[0] = 0.0f;
    state_estimate_x[1] = 0.0f;
    covariance_x[0] = 1.0f;
    covariance_x[1] = 1.0f;
    covariance_x[2] = 1.0f;
    covariance_x[3] = 1.0f;

    state_estimate_y[0] = 0.0f;
    state_estimate_y[1] = 0.0f;
    covariance_y[0] = 1.0f;
    covariance_y[1] = 1.0f;
    covariance_y[2] = 1.0f;
    covariance_y[3] = 1.0f;
  }

  /*Model for velocity estimation
   * state = [ velocity; acceleration]
   * Velocity_prediction = last_velocity_estimate + acceleration * dt
   * Acceleration_prediction = last_acceleration
   * model = Jacobian([vel_prediction; accel_prediction],state)
   *       = [1 dt ; 0 1];
   * */
  float model[4] =  {1.0f, 1.0f / fps , 0.0f , 1.0f};
  float process_noise[2] = {kalman_process_noise, kalman_process_noise};

  // Measurements from velocity_x of optical flow and acceleration directly from scaled accelerometers
  measurements_x[0] = *velocity_x;
  measurements_x[1] = acceleration_measurement[0];

  measurements_y[0] = *velocity_y;
  measurements_y[1] = acceleration_measurement[1];

  // 2D linear kalman filter
  kalman_filter_linear_2D_float(model, measurements_x, covariance_x, state_estimate_x, process_noise, measurement_noise);
  kalman_filter_linear_2D_float(model, measurements_y, covariance_y, state_estimate_y, process_noise, measurement_noise);

  *velocity_x = state_estimate_x[0];
  *velocity_y = state_estimate_y[0];
}

/**
 * Calculate the difference from start till finish
 * @param[in] *starttime The start time to calculate the difference from
 * @param[in] *finishtime The finish time to calculate the difference from
 * @return The difference in milliseconds
 */
static uint32_t timeval_diff(struct timeval *starttime, struct timeval *finishtime)
{
  uint32_t msec;
  msec = (finishtime->tv_sec - starttime->tv_sec) * 1000;
  msec += (finishtime->tv_usec - starttime->tv_usec) / 1000;
  return msec;
}

/**
 * Compare two flow vectors based on flow distance
 * Used for sorting.
 * @param[in] *a The first flow vector (should be vect flow_t)
 * @param[in] *b The second flow vector (should be vect flow_t)
 * @return Negative if b has more flow than a, 0 if the same and positive if a has more flow than b
 */
static int cmp_flow(const void *a, const void *b)
{
  const struct flow_t *a_p = (const struct flow_t *)a;
  const struct flow_t *b_p = (const struct flow_t *)b;
  return (a_p->flow_x * a_p->flow_x + a_p->flow_y * a_p->flow_y) - (b_p->flow_x * b_p->flow_x + b_p->flow_y *
         b_p->flow_y);
}


