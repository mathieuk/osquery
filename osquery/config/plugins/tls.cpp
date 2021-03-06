/*
 *  Copyright (c) 2014, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#include <vector>
#include <sstream>

#include <boost/property_tree/ptree.hpp>

#include <osquery/config.h>
#include <osquery/enroll.h>
#include <osquery/flags.h>
#include <osquery/registry.h>

#include "osquery/dispatcher/dispatcher.h"
#include "osquery/remote/requests.h"
#include "osquery/remote/transports/tls.h"
#include "osquery/remote/serializers/json.h"

#define CONFIG_TLS_MAX_ATTEMPTS 3

namespace pt = boost::property_tree;

namespace osquery {

/// Config retrieval TLS endpoint (path) using TLS hostname.
CLI_FLAG(string,
         config_tls_endpoint,
         "",
         "TLS/HTTPS endpoint for config retrieval");

/// Config polling/updating, only applies to TLS configurations.
FLAG(uint64,
     config_tls_refresh,
     0,
     "Optional interval in seconds to re-read configuration (min=10)");

class TLSConfigPlugin : public ConfigPlugin {
 public:
  Status setUp();
  Status genConfig(std::map<std::string, std::string>& config);
};

class TLSConfigRefreshRunner : public InternalRunnable {
 public:
  TLSConfigRefreshRunner() {}

  /// A simple wait/interruptible lock.
  void start();
};

REGISTER(TLSConfigPlugin, "config", "tls");

Status TLSConfigPlugin::setUp() {
  // If the initial configuration includes a non-0 refresh, start an additional
  // service that sleeps and periodically regenerates the configuration.
  if (FLAGS_config_tls_refresh >= 10) {
    Dispatcher::addService(std::make_shared<TLSConfigRefreshRunner>());
  }
  return Status(0, "OK");
}

Status makeTLSConfigRequest(const std::string& uri, pt::ptree& output) {
  // Make a request to the config endpoint, providing the node secret.
  pt::ptree params;
  params.put<std::string>("node_key", getNodeKey("tls"));

  auto request = Request<TLSTransport, JSONSerializer>(uri);
  auto status = request.call(params);
  if (!status.ok()) {
    return status;
  }

  // The call succeeded, store the enrolled key.
  status = request.getResponse(output);
  if (!status.ok()) {
    return status;
  }

  // Receive config or key rejection
  if (output.count("node_invalid") > 0) {
    return Status(1, "Config retrieval failed: Invalid node key");
  }
  return Status(0, "OK");
}

Status TLSConfigPlugin::genConfig(std::map<std::string, std::string>& config) {
  auto uri = "https://" + FLAGS_tls_hostname + FLAGS_config_tls_endpoint;
  VLOG(1) << "TLSConfigPlugin requesting a config from: " << uri;

  pt::ptree recv;
  for (size_t i = 1; i <= CONFIG_TLS_MAX_ATTEMPTS; i++) {
    auto status = makeTLSConfigRequest(uri, recv);
    if (status.ok()) {
      std::stringstream ss;
      try {
        pt::write_json(ss, recv, false);
      } catch (const pt::json_parser::json_parser_error& e) {
        // The response content could not be represented as JSON.
        continue;
      }

      config["tls_plugin"] = ss.str();
      return Status(0, "OK");
    } else if (i == CONFIG_TLS_MAX_ATTEMPTS) {
      break;
    }

    LOG(WARNING) << "Failed config retrieval from " << uri << " ("
                 << status.what() << ") retrying...";
    ::sleep(i * i);
  }

  return Status(1, "TLSConfigPlugin failed");
}

void TLSConfigRefreshRunner::start() {
  while (true) {
    // Cool off and time wait the configured period.
    // Apply this interruption initially as at t=0 the config was read.
    osquery::interruptableSleep(FLAGS_config_tls_refresh * 1000);

    // The config instance knows the TLS plugin is selected.
    Config::load();
  }
}
}
