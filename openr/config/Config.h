/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#if __has_include("filesystem")
#include <filesystem>
namespace fs = std::filesystem;
#else
#include <experimental/filesystem>
namespace fs = std::experimental::filesystem;
#endif
#include <folly/IPAddress.h>
#include <folly/io/async/SSLContext.h>
#include <openr/common/FileUtil.h>
#include <openr/common/MplsUtil.h>
#include <re2/re2.h>
#include <re2/set.h>
#include <thrift/lib/cpp2/server/ThriftServer.h>
#include <optional>

#include <openr/if/gen-cpp2/BgpConfig_types.h>
#include <openr/if/gen-cpp2/KvStore_types.h>
#include <openr/if/gen-cpp2/OpenrConfig_types.h>

namespace openr {

using PrefixAllocationParams = std::pair<folly::CIDRNetwork, uint8_t>;

class AreaConfiguration {
 public:
  explicit AreaConfiguration(thrift::AreaConfig const& area)
      : areaId_(*area.area_id()) {
    // parse non-optional fields
    neighborRegexSet_ = compileRegexSet(*area.neighbor_regexes());
    interfaceIncludeRegexSet_ =
        compileRegexSet(*area.include_interface_regexes());
    interfaceExcludeRegexSet_ =
        compileRegexSet(*area.exclude_interface_regexes());
    interfaceRedistRegexSet_ =
        compileRegexSet(*area.redistribute_interface_regexes());

    // parse optional fields
    if (auto nodeLabel = area.area_sr_node_label()) {
      srNodeLabel_ = *nodeLabel;
    }
    if (auto policyName = area.import_policy_name()) {
      importPolicyName_ = *policyName;
    }
  }

  std::string const&
  getAreaId() const {
    return areaId_;
  }

  std::optional<openr::thrift::SegmentRoutingNodeLabel>
  getNodeSegmentLabelConfig() const {
    return srNodeLabel_;
  }

  bool
  shouldDiscoverOnIface(std::string const& iface) const {
    return !interfaceExcludeRegexSet_->Match(iface, nullptr) &&
        interfaceIncludeRegexSet_->Match(iface, nullptr);
  }

  bool
  shouldPeerWithNeighbor(std::string const& neighbor) const {
    return neighborRegexSet_->Match(neighbor, nullptr);
  }

  bool
  shouldRedistributeIface(std::string const& iface) const {
    return interfaceRedistRegexSet_->Match(iface, nullptr);
  }

  std::optional<std::string>
  getImportPolicyName() const {
    return importPolicyName_;
  }

 private:
  const std::string areaId_;
  std::optional<openr::thrift::SegmentRoutingNodeLabel> srNodeLabel_{
      std::nullopt};

  std::optional<std::string> importPolicyName_{std::nullopt};

  // given a list of strings we will convert is to a compiled RE2::Set
  static std::shared_ptr<re2::RE2::Set> compileRegexSet(
      std::vector<std::string> const& strings);

  std::shared_ptr<re2::RE2::Set> neighborRegexSet_, interfaceIncludeRegexSet_,
      interfaceExcludeRegexSet_, interfaceRedistRegexSet_;
};

class Config {
 public:
  explicit Config(const std::string& configFile);
  explicit Config(thrift::OpenrConfig config) : config_(std::move(config)) {
    populateInternalDb();
  }

  static PrefixAllocationParams createPrefixAllocationParams(
      const std::string& seedPfxStr, uint8_t allocationPfxLen);

  //
  // config
  //
  const thrift::OpenrConfig&
  getConfig() const {
    return config_;
  }
  std::string getRunningConfig() const;

  const std::string&
  getNodeName() const {
    return *config_.node_name();
  }

  //
  // feature knobs
  //

  bool
  isV4Enabled() const {
    return config_.enable_v4().value_or(false);
  }

  bool
  isSegmentRoutingEnabled() const {
    return config_.enable_segment_routing().value_or(false);
  }

  bool
  isAdjacencyLabelsEnabled() const {
    if (isSegmentRoutingEnabled() && isSegmentRoutingConfigured()) {
      const auto& srConfig = getSegmentRoutingConfig();
      return srConfig.sr_adj_label().has_value() &&
          srConfig.sr_adj_label()->sr_adj_label_type() !=
          thrift::SegmentRoutingAdjLabelType::DISABLED;
    }
    return false;
  }

  bool
  isNetlinkFibHandlerEnabled() const {
    return config_.enable_netlink_fib_handler().value_or(false);
  }

  bool
  isFibServiceWaitingEnabled() const {
    return *config_.enable_fib_service_waiting();
  }

  bool
  isRibPolicyEnabled() const {
    return *config_.enable_rib_policy();
  }

  bool
  isBestRouteSelectionEnabled() const {
    return *config_.enable_best_route_selection();
  }

  bool
  isLogSubmissionEnabled() const {
    return *getMonitorConfig().enable_event_log_submission();
  }

  bool
  isV4OverV6NexthopEnabled() const {
    return config_.v4_over_v6_nexthop().value_or(false);
  }

  bool
  isUcmpEnabled() const {
    return *config_.enable_ucmp();
  }

  bool
  isDryrun() const {
    return config_.dryrun().value_or(false);
  }

  //
  // area
  //

  void populateAreaConfig();

  void checkAdjacencyLabelConfig(
      const openr::thrift::AreaConfig& areaConf) const;

  void checkNodeSegmentLabelConfig(
      const openr::thrift::AreaConfig& areaConf) const;

  const std::unordered_map<std::string, AreaConfiguration>&
  getAreas() const {
    return areaConfigs_;
  }

  std::unordered_set<std::string>
  getAreaIds() const {
    std::unordered_set<std::string> ids;
    for (auto const& [id, _] : areaConfigs_) {
      ids.insert(id);
    }
    return ids;
  }

  //
  // spark
  //
  const thrift::SparkConfig&
  getSparkConfig() const {
    return *config_.spark_config();
  }

  //
  // kvstore
  //
  thrift::KvStoreConfig toThriftKvStoreConfig() const;

  const thrift::KvstoreConfig&
  getKvStoreConfig() const {
    return *config_.kvstore_config();
  }

  std::chrono::milliseconds
  getKvStoreKeyTtl() const {
    return std::chrono::milliseconds(*config_.kvstore_config()->key_ttl_ms());
  }

  //
  // decision
  //
  bool
  isBgpRouteProgrammingEnabled() const {
    return *config_.decision_config()->enable_bgp_route_programming();
  }

  //
  // link monitor
  //
  const thrift::LinkMonitorConfig&
  getLinkMonitorConfig() const {
    return *config_.link_monitor_config();
  }

  //
  // segment routing
  //
  const thrift::SegmentRoutingConfig&
  getSegmentRoutingConfig() const {
    return *config_.segment_routing_config();
  }

  const thrift::SegmentRoutingAdjLabel&
  getAdjSegmentLabels() const {
    CHECK(
        config_.segment_routing_config().has_value() and
        config_.segment_routing_config()->sr_adj_label().has_value());
    return *config_.segment_routing_config()->sr_adj_label();
  }

  bool
  isSegmentRoutingConfigured() const {
    return config_.segment_routing_config().has_value();
  }

  //
  // prefix Allocation
  //
  bool
  isPrefixAllocationEnabled() const {
    return config_.enable_prefix_allocation().value_or(false);
  }

  const thrift::PrefixAllocationConfig&
  getPrefixAllocationConfig() const {
    CHECK(isPrefixAllocationEnabled());
    return *config_.prefix_allocation_config();
  }

  //
  // dispatcher
  //
  bool
  isKvStoreDispatcherEnabled() const {
    return config_.enable_kvstore_dispatcher().value();
  }

  PrefixAllocationParams
  getPrefixAllocationParams() const {
    CHECK(isPrefixAllocationEnabled());
    return *prefixAllocationParams_;
  }

  // MPLS labels
  bool
  isLabelRangeValid(thrift::LabelRange range) const {
    if (not isMplsLabelValid(*range.start_label())) {
      return false;
    }

    if (not isMplsLabelValid(*range.end_label())) {
      return false;
    }

    if (*range.start_label() > *range.end_label()) {
      return false;
    }

    return true;
  }

  //
  // bgp peering
  //
  bool
  isBgpPeeringEnabled() const {
    return config_.enable_bgp_peering().value_or(false);
  }

  const thrift::BgpConfig&
  getBgpConfig() const {
    CHECK(isBgpPeeringEnabled());
    return *config_.bgp_config();
  }

  const thrift::BgpRouteTranslationConfig&
  getBgpTranslationConfig() const {
    CHECK(isBgpPeeringEnabled());
    return *config_.bgp_translation_config();
  }

  //
  // watch dog
  //
  bool
  isWatchdogEnabled() const {
    return config_.enable_watchdog().value_or(false);
  }

  const thrift::WatchdogConfig&
  getWatchdogConfig() const {
    CHECK(isWatchdogEnabled());
    return *config_.watchdog_config();
  }

  //
  // monitor
  //
  const thrift::MonitorConfig&
  getMonitorConfig() const {
    return *config_.monitor_config();
  }

  //
  // policy
  //
  std::optional<neteng::config::routing_policy::PolicyConfig>
  getAreaPolicies() const {
    return config_.area_policies().to_optional();
  }

  //
  // thrift server
  //
  const thrift::ThriftServerConfig
  getThriftServerConfig() const {
    return *config_.thrift_server();
  }
  bool
  isSecureThriftServerEnabled() const {
    return *getThriftServerConfig().enable_secure_thrift_server();
  }

  bool
  isNonDefaultVrfThriftServerEnabled() const {
    return getThriftServerConfig()
        .enable_non_default_vrf_thrift_server()
        .value_or(false);
  }

  const std::string
  getSSLCertPath() const {
    auto certPath = getThriftServerConfig().x509_cert_path();
    if ((not certPath) && isSecureThriftServerEnabled()) {
      throw std::invalid_argument(
          "enable_secure_thrift_server = true, but x509_cert_path is empty");
    }
    return certPath.value();
  }

  const std::string
  getSSLEccCurve() const {
    auto eccCurve = getThriftServerConfig().ecc_curve_name();
    if ((not eccCurve) && isSecureThriftServerEnabled()) {
      throw std::invalid_argument(
          "enable_secure_thrift_server = true, but ecc_curve_name is empty");
    }
    return eccCurve.value();
  }

  const std::string
  getSSLCaPath() const {
    auto caPath = getThriftServerConfig().x509_ca_path();
    if ((not caPath) && isSecureThriftServerEnabled()) {
      throw std::invalid_argument(
          "enable_secure_thrift_server = true, but x509_ca_path is empty");
    }
    return caPath.value();
  }

  const std::string
  getSSLKeyPath() const {
    std::string keyPath;
    const auto& keyPathConfig = getThriftServerConfig().x509_key_path();

    // If unspecified x509_key_path, will use x509_cert_path
    if (keyPathConfig) {
      keyPath = keyPathConfig.value();
    } else {
      keyPath = getSSLCertPath();
    }
    return keyPath;
  }

  const std::string
  getSSLSeedPath() const {
    auto seedPath = getThriftServerConfig().ticket_seed_path();
    if ((not seedPath) && isSecureThriftServerEnabled()) {
      throw std::invalid_argument(
          "enable_secure_thrift_server = true, but ticket_seed_path is empty");
    }
    return seedPath.value();
  }

  const std::string
  getSSLAcceptablePeers() const {
    // If unspecified, will use accept connection from any authenticated peer
    return getThriftServerConfig().acceptable_peers().value_or("");
  }

  folly::SSLContext::VerifyClientCertificate
  getSSLContextVerifyType() const {
    // Get the verify_client_type config
    auto mode = getThriftServerConfig().verify_client_type().value_or(
        thrift::VerifyClientType::DO_NOT_REQUEST);

    // Set the folly::SSLContext::VerifyClientCertificate for thrift server
    switch (mode) {
    case thrift::VerifyClientType::ALWAYS:
      return folly::SSLContext::VerifyClientCertificate::ALWAYS;

    case thrift::VerifyClientType::IF_PRESENTED:
      return folly::SSLContext::VerifyClientCertificate::IF_PRESENTED;

    default:
      return folly::SSLContext::VerifyClientCertificate::DO_NOT_REQUEST;
    }
  }

  apache::thrift::SSLPolicy
  getSSLThriftPolicy() const {
    // Get the verify_client_type config
    auto mode = getThriftServerConfig().verify_client_type().value_or(
        thrift::VerifyClientType::DO_NOT_REQUEST);

    // Set the apache::thrift::SSLPolicy for starting thrift server
    switch (mode) {
    case thrift::VerifyClientType::ALWAYS:
      return apache::thrift::SSLPolicy::REQUIRED;

    case thrift::VerifyClientType::IF_PRESENTED:
      return apache::thrift::SSLPolicy::PERMITTED;

    default:
      return apache::thrift::SSLPolicy::DISABLED;
    }
  }

  //
  // thrift client
  //
  std::optional<thrift::ThriftClientConfig>
  getThriftClientConfig() const {
    return config_.thrift_client().to_optional();
  }

  //
  // VIP thrift injection service
  //
  bool
  isVipServiceEnabled() const {
    return config_.enable_vip_service().value_or(false);
  }

  //
  // VIP thrift injection config
  //
  const vipconfig::config::VipServiceConfig&
  getVipServiceConfig() const {
    CHECK(isVipServiceEnabled());
    return *config_.vip_service_config();
  }

  /*
   * [Drain Status]
   *
   * Based on configured file flag path, Open/R will determine the node
   * is in either:
   *  - UNDRAINED;
   *  - DRAINED with either SOFTDRAINED(metric bump) or HARDDRAINED(overloaded);
   */
  bool
  isUndrainedPathExist() const {
    auto undrainedFlagPath = config_.undrained_flag_path();
    return undrainedFlagPath and fs::exists(*undrainedFlagPath);
  }

  bool
  isAssumeDrained() const {
    // Do not assume drain if the undrained_flag_path is set and the file exists
    if (isUndrainedPathExist()) {
      return false;
    }
    return *config_.assume_drained();
  }

  bool
  isSoftdrainEnabled() const {
    return *config_.enable_soft_drain();
  }

  int64_t
  getNodeMetricIncrement() const {
    return *config_.softdrained_node_increment();
  }

  //
  // Memory profiling
  //
  bool
  isMemoryProfilingEnabled() const {
    auto memProfileConf = config_.memory_profiling_config();
    return memProfileConf.has_value() and
        memProfileConf.value().enable_memory_profiling().value();
  }

  std::chrono::seconds
  getMemoryProfilingInterval() const {
    if (isMemoryProfilingEnabled()) {
      return std::chrono::seconds(
          *config_.memory_profiling_config()->heap_dump_interval_s());
    } else {
      throw std::invalid_argument(
          "Trying to set memory profile timer with heap_dump_interval_s, but enable_memory_profiling = false");
    }
  }

 private:
  void populateInternalDb();

  // validate KvStore confg
  void checkKvStoreConfig() const;

  // validate Decision module config
  void checkDecisionConfig() const;

  // validate Spark config
  void checkSparkConfig() const;

  // validate Monitor config
  void checkMonitorConfig() const;

  // validate Link Monitor config
  void checkLinkMonitorConfig() const;

  // validate Segment Routing config
  void checkSegmentRoutingConfig() const;

  // validate Prefix Allocation config
  void checkPrefixAllocationConfig();

  // validate VipService Config
  void checkVipServiceConfig() const;

  // validate BGP Peering config and BGP Translation Config
  void checkBgpPeeringConfig();

  // validate thrift server config
  void checkThriftServerConfig() const;

  // thrift config
  thrift::OpenrConfig config_;
  // prefix allocation
  folly::Optional<PrefixAllocationParams> prefixAllocationParams_{folly::none};

  // areaId -> neighbor regex and interface regex mapped
  std::unordered_map<std::string /* areaId */, AreaConfiguration> areaConfigs_;

// per class placeholder for test code
// only need to be setup once here
#ifdef Config_TEST_FRIENDS
  Config_TEST_FRIENDS
#endif
};

} // namespace openr
