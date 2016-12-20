/*ckwg +29
 * Copyright 2016 by Kitware, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  * Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 *  * Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 *  * Neither name of Kitware, Inc. nor the names of any contributors may be used
 *    to endorse or promote products derived from this software without specific
 *    prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHORS OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * \file
 * \brief Apply Ground Control Points utility
 */

#include <iostream>
#include <iomanip>
#include <fstream>
#include <sstream>
#include <exception>
#include <string>
#include <vector>

#include <vital/vital_foreach.h>
#include <vital/config/config_block.h>
#include <vital/config/config_block_io.h>

#include <vital/algo/estimate_canonical_transform.h>
#include <vital/algo/estimate_similarity_transform.h>
#include <vital/algo/geo_map.h>
#include <vital/algo/triangulate_landmarks.h>
#include <vital/algorithm_plugin_manager.h>
#include <vital/exceptions.h>
#include <vital/io/camera_io.h>
#include <vital/io/eigen_io.h>
#include <vital/io/landmark_map_io.h>
#include <vital/io/track_set_io.h>
#include <vital/types/track_set.h>
#include <vital/vital_types.h>
#include <vital/util/cpu_timer.h>
#include <vital/util/get_paths.h>

#include <kwiversys/SystemTools.hxx>
#include <kwiversys/CommandLineArguments.hxx>
#include <kwiversys/Directory.hxx>

#include <arrows/core/metrics.h>
#include <arrows/core/transform.h>

#include <maptk/geo_reference_points_io.h>
#include <maptk/ins_data_io.h>
#include <maptk/local_geo_cs.h>
#include <maptk/version.h>

typedef kwiversys::SystemTools     ST;

static kwiver::vital::logger_handle_t main_logger( kwiver::vital::get_logger( "apply_gcp_tool" ) );

static kwiver::vital::config_block_sptr default_config()
{

  kwiver::vital::config_block_sptr config = kwiver::vital::config_block::empty_config("apply_gcp_tool");

  config->set_value("image_list_file", "",
                    "Path to the input image list file used to generated the "
                    "input tracks.");

  config->set_value("input_ply_file", "",
                    "Path to the PLY file from which to read 3D landmark points");

  config->set_value("input_krtd_files", "",
                    "A directory containing input KRTD camera files, or a text "
                    "file containing a newline-separated list of KRTD files.\n"
                    "\n"
                    "This is optional, leave blank to ignore.");

  config->set_value("input_reference_points_file", "",
                    "File containing reference points to use for reprojection "
                    "of results into the geographic coordinate system.\n"
                    "\n"
                    "This option is NOT mutually exclusive with input_*_files "
                    "options when using an st_estimator. When both this and "
                    "another input files option are specified, use of the "
                    "reference file is given priority over the input "
                    "cameras.\n"
                    "\n"
                    "Reference points file format (lm=landmark, tNsM=track N state M):\n"
                    "\tlm1.x lm1.y lm1.z t1s1.frame t1s1.x t1s1.y t1s2.frame t1s2.x t1s2.y ...\n"
                    "\tlm2.x lm2.y lm2.z t2s1.frame t2s1.x t2s1.y t2s2.frame t2s2.x t2s2.y ...\n"
                    "\t...\n"
                    "\n"
                    "At least 3 landmarks must be given, with at least 2 "
                    "track states recorded for each landmark, for "
                    "transformation estimation to converge, however more of "
                    "each is recommended.\n"
                    "\n"
                    "Landmark z position, or altitude, should be provided in meters.");

  config->set_value("geo_origin_file", "output/geo_origin.txt",
                    "This file contains the geographical location of the origin "
                    "of the local cartesian coordinate system used in the camera "
                    "and landmark files.  This file is use for input and output. "
                    "If the files exists it will be read to define the origin. "
                    "If the file does not exist an origin will be computed from "
                    "geographic metadata provided and written to this file. "
                    "The file format is ASCII (degrees, meters):\n"
                    "latitude longitude altitude");

  config->set_value("output_ply_file", "output/landmarks.ply",
                    "Path to the output PLY file in which to write "
                    "resulting 3D landmark points");

  config->set_value("output_pos_dir", "output/pos",
                    "A directory in which to write the output POS files.");

  config->set_value("output_krtd_dir", "output/krtd",
                    "A directory in which to write the output KRTD files.");

  kwiver::vital::algo::triangulate_landmarks::get_nested_algo_configuration("triangulator", config,
                        kwiver::vital::algo::triangulate_landmarks_sptr());
  kwiver::vital::algo::geo_map::get_nested_algo_configuration("geo_mapper", config,
                                               kwiver::vital::algo::geo_map_sptr());
  kwiver::vital::algo::estimate_similarity_transform::get_nested_algo_configuration("st_estimator", config,
                                                                     kwiver::vital::algo::estimate_similarity_transform_sptr());
  kwiver::vital::algo::estimate_canonical_transform::get_nested_algo_configuration("can_tfm_estimator", config,
                                                                     kwiver::vital::algo::estimate_canonical_transform_sptr());

  return config;
}


// ------------------------------------------------------------------
static bool check_config(kwiver::vital::config_block_sptr config)
{
  bool config_valid = true;

#define MAPTK_CONFIG_FAIL(msg) \
  LOG_ERROR(main_logger, "Config Check Fail: " << msg); \
  config_valid = false

  if (!config->has_value("image_list_file"))
  {
    MAPTK_CONFIG_FAIL("Not given an image list file");
  }
  else if (! ST::FileExists( config->get_value<std::string>("image_list_file"), true ) )
  {
    MAPTK_CONFIG_FAIL("Given image list file path doesn't point to an existing file.");
  }

  // Checking input cameras and reference points file existance.
  if (config->get_value<std::string>("input_krtd_files", "") != "")
  {
    if (! ST::FileExists(config->get_value<std::string>("input_krtd_files")))
    {
      MAPTK_CONFIG_FAIL("KRTD input path given, but does not point to an existing location.");
    }
  }
  if (config->get_value<std::string>("input_reference_points_file", "") != "")
  {
    if (! ST::FileExists(config->get_value<std::string>("input_reference_points_file"), true ))
    {
      MAPTK_CONFIG_FAIL("Path given for input reference points file does not exist.");
    }
  }

  if (!kwiver::vital::algo::triangulate_landmarks::check_nested_algo_configuration("triangulator", config))
  {
    MAPTK_CONFIG_FAIL("Failed config check in triangulator algorithm.");
  }
  if (!kwiver::vital::algo::geo_map::check_nested_algo_configuration("geo_mapper", config))
  {
    MAPTK_CONFIG_FAIL("Failed config check in geo_mapper algorithm.");
  }
  if (config->has_value("st_estimator:type") && config->get_value<std::string>("st_estimator:type") != "")
  {
    if (!kwiver::vital::algo::estimate_similarity_transform::check_nested_algo_configuration("st_estimator", config))
    {
      MAPTK_CONFIG_FAIL("Failed config check in st_estimator algorithm.");
    }
  }
  if (config->has_value("can_tfm_estimator:type") && config->get_value<std::string>("can_tfm_estimator:type") != "")
  {
    if (!kwiver::vital::algo::estimate_canonical_transform::check_nested_algo_configuration("can_tfm_estimator", config))
    {
      MAPTK_CONFIG_FAIL("Failed config check in can_tfm_estimator algorithm.");
    }
  }


#undef MAPTK_CONFIG_FAIL

  return config_valid;
}

// ------------------------------------------------------------------
// return a sorted list of files in a directory
std::vector< kwiver::vital::path_t >
files_in_dir(kwiver::vital::path_t const& vdir)
{
  std::vector< kwiver::vital::path_t > files;

  kwiversys::Directory dir;
  if ( 0 == dir.Load( vdir ) )
  {
    LOG_WARN(main_logger, "Could not access directory \"" << vdir << "\"");
    return files;
  }

  unsigned long num_files = dir.GetNumberOfFiles();
  for ( unsigned long i = 0; i < num_files; i++)
  {
    files.push_back( vdir + '/' + dir.GetFile( i ) );
  }

  std::sort( files.begin(), files.end() );
  return files;
}


// ------------------------------------------------------------------
/// Return a list of file paths either from a directory of files or from a
/// list of file paths
///
/// Returns false if we were given a file list and the file could not be
/// opened. Otherwise returns true.
bool
resolve_files(kwiver::vital::path_t const &p, std::vector< kwiver::vital::path_t > &files)
{
  if ( ST::FileIsDirectory( p) )
  {
    files = files_in_dir(p);
  }
  else
  {
    std::ifstream ifs(p.c_str());
    if (!ifs)
    {
      return false;
    }
    for (std::string line; std::getline(ifs, line);)
    {
      files.push_back(line);
    }
  }
  return true;
}


// Load input KRTD cameras from file, matching against the given image
// filename map. Returns false if failure occurred.
bool
load_input_cameras_krtd(kwiver::vital::config_block_sptr config,
                        std::map<std::string, kwiver::vital::frame_id_t> const& filename2frame,
                        kwiver::vital::camera_map::map_camera_t & input_cameras)
{
  kwiver::vital::scoped_cpu_timer t( "Initializing cameras from KRTD files" );

  // Collect files
  std::string krtd_files = config->get_value<std::string>("input_krtd_files");
  std::vector< kwiver::vital::path_t > files;
  if (!resolve_files(krtd_files, files))
  {
    LOG_ERROR(main_logger, "Could not open KRTD file list.");
    return false;
  }

  // Associating KRTD files to the frame ID of a matching input image based
  // on file stem naming.
  LOG_INFO(main_logger, "loading KRTD input camera files");
  kwiver::vital::camera_map::map_camera_t krtd_cams;
  std::map<std::string, kwiver::vital::frame_id_t>::const_iterator it;
  VITAL_FOREACH(kwiver::vital::path_t const& fpath, files)
  {
    std::string krtd_file_stem = ST::GetFilenameWithoutLastExtension( fpath );
    it = filename2frame.find(krtd_file_stem);
    if (it != filename2frame.end())
    {
      kwiver::vital::camera_sptr cam = kwiver::vital::read_krtd_file(fpath);
      krtd_cams[it->second] = cam;
    }
  }

  // if krtd_map is empty, then there were no input krtd files that matched
  // input imagery.
  if (krtd_cams.empty())
  {
    LOG_ERROR(main_logger, "No KRTD files from input set match input image "
                           << "frames. Check KRTD input files!");
    return false;
  }
  else
  {
    // Warning if loaded KRTD camera set is sparse compared to input imagery
    // TODO: generated interpolated cameras for missing KRTD files.
    if (filename2frame.size() != krtd_cams.size())
    {
      LOG_WARN(main_logger, "Input KRTD camera set is sparse compared to input "
                            << "imagery! (there wasn't a matching KRTD input file for "
                            << "every input image file)");
    }
    input_cameras = krtd_cams;
  }
  return true;
}


static int maptk_main(int argc, char const* argv[])
{
  static bool        opt_help(false);
  static std::string opt_config;
  static std::string opt_out_config;

  kwiversys::CommandLineArguments arg;

  arg.Initialize( argc, argv );
  typedef kwiversys::CommandLineArguments argT;

  arg.AddArgument( "--help",        argT::NO_ARGUMENT, &opt_help, "Display usage information" );
  arg.AddArgument( "--config",      argT::SPACE_ARGUMENT, &opt_config, "Configuration file for tool" );
  arg.AddArgument( "-c",            argT::SPACE_ARGUMENT, &opt_config, "Configuration file for tool" );
  arg.AddArgument( "--output-config", argT::SPACE_ARGUMENT, &opt_out_config,
                   "Output a configuration. This may be seeded with a configuration file from -c/--config." );
  arg.AddArgument( "-o",            argT::SPACE_ARGUMENT, &opt_out_config,
                   "Output a configuration. This may be seeded with a configuration file from -c/--config." );

    if ( ! arg.Parse() )
  {
    LOG_ERROR(main_logger, "Problem parsing arguments");
    return EXIT_FAILURE;
  }

  if ( opt_help )
  {
    std::cout
      << "USAGE: " << argv[0] << " [OPTS]\n\n"
      << "Options:"
      << arg.GetHelp() << std::endl;
    return EXIT_SUCCESS;
  }

  // register the algorithm implementations
  std::string rel_plugin_path = kwiver::vital::get_executable_path() + "/../lib/maptk";
  kwiver::vital::algorithm_plugin_manager::instance().add_search_path(rel_plugin_path);
  kwiver::vital::algorithm_plugin_manager::instance().register_plugins();

  // Set config to algo chain
  // Get config from algo chain after set
  // Check config validity, store result
  //
  // If -o/--output-config given,
  //   output config result and notify of current (in)validity
  // Else error if provided config not valid.

  // Set up top level configuration w/ defaults where applicable.
  kwiver::vital::config_block_sptr config = kwiver::vital::config_block::empty_config();
  kwiver::vital::algo::triangulate_landmarks_sptr triangulator;
  kwiver::vital::algo::geo_map_sptr geo_mapper;
  kwiver::vital::algo::estimate_similarity_transform_sptr st_estimator;
  kwiver::vital::algo::estimate_canonical_transform_sptr can_tfm_estimator;

  // If -c/--config given, read in confg file, merge in with default just generated
  if( ! opt_config.empty() )
  {
    const std::string prefix = kwiver::vital::get_executable_path() + "/..";
    config->merge_config(kwiver::vital::read_config_file(opt_config, "maptk",
                                                         MAPTK_VERSION, prefix));
  }


  kwiver::vital::algo::triangulate_landmarks::set_nested_algo_configuration("triangulator", config, triangulator);
  kwiver::vital::algo::geo_map::set_nested_algo_configuration("geo_mapper", config, geo_mapper);
  kwiver::vital::algo::estimate_similarity_transform::set_nested_algo_configuration("st_estimator", config, st_estimator);
  kwiver::vital::algo::estimate_canonical_transform::set_nested_algo_configuration("can_tfm_estimator", config, can_tfm_estimator);

  kwiver::vital::config_block_sptr dflt_config = default_config();
  dflt_config->merge_config(config);
  config = dflt_config;

  bool valid_config = check_config(config);

  if( ! opt_out_config.empty() )
  {
    kwiver::vital::algo::triangulate_landmarks::get_nested_algo_configuration("triangulator", config, triangulator);
    kwiver::vital::algo::geo_map::get_nested_algo_configuration("geo_mapper", config, geo_mapper);
    kwiver::vital::algo::estimate_similarity_transform::get_nested_algo_configuration("st_estimator", config, st_estimator);
    kwiver::vital::algo::estimate_canonical_transform::get_nested_algo_configuration("can_tfm_estimator", config, can_tfm_estimator);

    write_config_file(config, opt_out_config );
    if(valid_config)
    {
      LOG_INFO(main_logger, "Configuration file contained valid parameters"
                            << " and may be used for running");
    }
    else
    {
      LOG_WARN(main_logger, "Configuration deemed not valid.");
    }
    return EXIT_SUCCESS;
  }
  else if(!valid_config)
  {
    LOG_ERROR(main_logger, "Configuration not valid.");
    return EXIT_FAILURE;
  }

  //
  // Read in image list file
  //
  // Also creating helper structures (i.e. frameID-to-filename and vise versa
  // maps).
  //
  std::string image_list_file = config->get_value<std::string>("image_list_file");
  std::ifstream image_list_ifs(image_list_file.c_str());
  if (!image_list_ifs)
  {
    LOG_ERROR(main_logger, "Could not open image list file!");
    return EXIT_FAILURE;
  }
  std::vector<kwiver::vital::path_t> image_files;
  for (std::string line; std::getline(image_list_ifs, line); )
  {
    image_files.push_back(line);
  }
  // Since input tracks were generated over these frames, we can assume that
  // the frames are "in order" in that tracking followed this list in this given
  // order. As this is a single list, we assume that there are no gaps (same
  // assumptions as makde in tracking).
  // Creating forward and revese mappings for frame to file stem-name.
  std::vector<std::string> frame2filename;  // valid since we are assuming no frame gaps
  std::map<std::string, kwiver::vital::frame_id_t> filename2frame;
  VITAL_FOREACH(kwiver::vital::path_t i_file, image_files)
  {
    std::string i_file_stem = ST::GetFilenameWithoutLastExtension( i_file );
    filename2frame[i_file_stem] = static_cast<kwiver::vital::frame_id_t>(frame2filename.size());
    frame2filename.push_back(i_file_stem);
  }

  //
  // Create the local coordinate system
  //
  kwiver::maptk::local_geo_cs local_cs(geo_mapper);
  bool geo_origin_loaded_from_file = false;
  if (config->get_value<std::string>("geo_origin_file", "") != "")
  {
    kwiver::vital::path_t geo_origin_file = config->get_value<kwiver::vital::path_t>("geo_origin_file");
    // load the coordinates from a file if it exists
    if (ST::FileExists(geo_origin_file, true))
    {
      std::ifstream ifs(geo_origin_file);
      double lat, lon, alt;
      ifs >> lat >> lon >> alt;
      LOG_INFO(main_logger, "Loaded origin point: "
                            << lat << ", " << lon << ", " << alt);
      double x,y;
      int zone;
      bool is_north_hemi;
      local_cs.geo_map_algo()->latlon_to_utm(lat, lon, x, y, zone, is_north_hemi);
      local_cs.set_utm_origin_zone(zone);
      local_cs.set_utm_origin(kwiver::vital::vector_3d(x, y, alt));
      geo_origin_loaded_from_file = true;
    }
  }


  //
  // Load Cameras and Landmarks
  //
  kwiver::vital::camera_map::map_camera_t input_cameras;
  if (!load_input_cameras_krtd(config, filename2frame, input_cameras))
  {
    LOG_ERROR(main_logger, "Failed to load input cameras");
    return EXIT_FAILURE;
  }

  kwiver::vital::landmark_map_sptr lm_map;
  if( config->has_value("input_ply_file") )
  {
    std::string ply_file = config->get_value<std::string>("input_ply_file");
    lm_map = kwiver::vital::read_ply_file(ply_file);
  }


  // Copy input cameras into main camera map
  kwiver::vital::camera_map::map_camera_t cameras;
  kwiver::vital::camera_map_sptr input_cam_map(new kwiver::vital::simple_camera_map(input_cameras));
  if (input_cameras.size() != 0)
  {
    VITAL_FOREACH(kwiver::vital::camera_map::map_camera_t::value_type &v, input_cameras)
    {
      cameras[v.first] = v.second->clone();
    }
  }
  kwiver::vital::camera_map_sptr cam_map;
  if(!cameras.empty())
  {
    cam_map = kwiver::vital::camera_map_sptr(new kwiver::vital::simple_camera_map(cameras));
  }

  kwiver::vital::landmark_map_sptr reference_landmarks(new kwiver::vital::simple_landmark_map());
  kwiver::vital::track_set_sptr reference_tracks(new kwiver::vital::simple_track_set());
  if (config->get_value<std::string>("input_reference_points_file", "") != "")
  {
    kwiver::vital::path_t ref_file = config->get_value<kwiver::vital::path_t>("input_reference_points_file");

    // Load up landmarks and assocaited tracks from file, (re)initializing
    // local coordinate system object to the reference.
    kwiver::maptk::load_reference_file(ref_file, local_cs, reference_landmarks, reference_tracks);
  }

  // if we computed an origin that was not loaded from a file
  if (local_cs.utm_origin_zone() >= 0 &&
      !geo_origin_loaded_from_file)
  {
    // write out the origin of the local coordinate system
    double easting = local_cs.utm_origin()[0];
    double northing = local_cs.utm_origin()[1];
    double altitude = local_cs.utm_origin()[2];
    int zone = local_cs.utm_origin_zone();
    double lat, lon;
    local_cs.geo_map_algo()->utm_to_latlon(easting, northing, zone, true, lat, lon);
    if (config->get_value<std::string>("geo_origin_file", "") != "")
    {
      kwiver::vital::path_t geo_origin_file = config->get_value<kwiver::vital::path_t>("geo_origin_file");
      std::ofstream ofs(geo_origin_file);
      if (ofs)
      {
        LOG_INFO(main_logger, "Saving local coordinate origin to " << geo_origin_file);
        ofs << std::setprecision(12) << lat << " " << lon << " " << altitude;
      }
    }
    LOG_INFO(main_logger, "Local coordinate origin: " << std::setprecision(12)
                                                      << lat << ", "
                                                      << lon << ", "
                                                      << altitude);
  }

  //
  // Adjust cameras/landmarks based on input cameras/reference points
  //
  // If we were given POS files / reference points as input, compute a
  // similarity transform from the refined cameras to the POS file / reference
  // point structures. Then, apply the estimated transform to the refined
  // camera positions and landmarks.
  //
  // The effect of this is to put the refined cameras and landmarks into the
  // same coordinate system as the input cameras / reference points.
  //
  if (st_estimator || can_tfm_estimator)
  {
    kwiver::vital::scoped_cpu_timer t_1( "--> st estimation and application" );
    LOG_INFO(main_logger, "Estimating similarity transform from post-SBA to original space");

    // initialize identity transform
    kwiver::vital::similarity_d sim_transform;

    // Prioritize use of reference landmarks/tracks over use of POS files for
    // transformation out of SBA-space.
    if (reference_landmarks->size() > 0 && reference_tracks->size() > 0)
    {
      kwiver::vital::scoped_cpu_timer t_2( "similarity transform estimation from ref file" );
      LOG_INFO(main_logger, "Using reference landmarks/tracks");

      // Generate corresponding landmarks in SBA-space based on transformed
      //    cameras and reference landmarks/tracks via triangulation.
      LOG_INFO(main_logger, "Triangulating SBA-space reference landmarks from "
                            << "reference tracks and post-SBA cameras");
      kwiver::vital::landmark_map_sptr sba_space_landmarks(new kwiver::vital::simple_landmark_map(reference_landmarks->landmarks()));
      triangulator->triangulate(cam_map, reference_tracks, sba_space_landmarks);
      if (sba_space_landmarks->size() < reference_landmarks->size())
      {
        LOG_WARN(main_logger, "Only " << sba_space_landmarks->size()
                              << " out of " << reference_landmarks->size()
                              << " reference points triangulated");
      }

      double post_tri_rmse = kwiver::arrows::reprojection_rmse(cam_map->cameras(),
                                                              sba_space_landmarks->landmarks(),
                                                              reference_tracks->tracks());
      LOG_DEBUG(main_logger, "Post-triangulation RMSE: " << post_tri_rmse);

      // Estimate ST from sba-space to reference space.
      LOG_INFO(main_logger, "Estimating transform to reference landmarks (from "
                            << "SBA-space ref landmarks)");
      sim_transform = st_estimator->estimate_transform(sba_space_landmarks, reference_landmarks);
    }
    else if (can_tfm_estimator)
    {
      // In the absence of other information, use a canonical transformation
      sim_transform = can_tfm_estimator->estimate_transform(cam_map, lm_map);
    }

    LOG_DEBUG(main_logger, "Estimated Transformation: " << sim_transform);

    // apply to cameras and landmarks
    LOG_INFO(main_logger, "Applying transform to cameras and landmarks");
    cam_map = kwiver::arrows::transform(cam_map, sim_transform);
    lm_map = kwiver::arrows::transform(lm_map, sim_transform);
  }

  //
  // Write the output PLY file
  //
  if( config->has_value("output_ply_file") )
  {
    kwiver::vital::scoped_cpu_timer t( "writing output PLY file" );
    std::string ply_file = config->get_value<std::string>("output_ply_file");
    write_ply_file(lm_map, ply_file);
  }

  //
  // Write the output POS files
  //
  if( config->has_value("output_pos_dir") )
  {
    LOG_INFO(main_logger, "Writing output POS files");
    kwiver::vital::scoped_cpu_timer t( "--> Writing output POS files" );

    kwiver::vital::path_t pos_dir = config->get_value<std::string>("output_pos_dir");
    // Create INS data from adjusted cameras for POS file output.
    typedef std::map<kwiver::vital::frame_id_t, kwiver::maptk::ins_data> ins_map_t;
    ins_map_t ins_map;
    update_ins_from_cameras(cam_map->cameras(), local_cs, ins_map);
    VITAL_FOREACH(const ins_map_t::value_type& p, ins_map)
    {
      kwiver::vital::path_t out_pos_file = pos_dir + "/" + frame2filename[p.first] + ".pos";
      write_pos_file( p.second, out_pos_file);
    }
    if (ins_map.size() == 0)
    {
      LOG_WARN(main_logger, "INS map empty, no output POS files written");
    }
  }

  //
  // Write the output KRTD files
  //
  if( config->has_value("output_krtd_dir") )
  {
    LOG_INFO(main_logger, "Writing output KRTD files");
    kwiver::vital::scoped_cpu_timer t("--> Writing output KRTD files" );

    kwiver::vital::path_t krtd_dir = config->get_value<std::string>("output_krtd_dir");
    typedef kwiver::vital::camera_map::map_camera_t::value_type cam_map_val_t;
    VITAL_FOREACH(cam_map_val_t const& p, cam_map->cameras())
    {
      kwiver::vital::path_t out_krtd_file = krtd_dir + "/" + frame2filename[p.first] + ".krtd";
      write_krtd_file( *p.second, out_krtd_file );
    }
  }

  return EXIT_SUCCESS;
}


int main(int argc, char const* argv[])
{
  try
  {
    return maptk_main(argc, argv);
  }
  catch (std::exception const& e)
  {
    LOG_ERROR(main_logger, "Exception caught: " << e.what());

    return EXIT_FAILURE;
  }
  catch (...)
  {
    LOG_ERROR(main_logger, "Unknown exception caught");

    return EXIT_FAILURE;
  }
}
