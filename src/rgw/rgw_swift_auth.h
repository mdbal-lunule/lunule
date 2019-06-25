// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_RGW_SWIFT_AUTH_H
#define CEPH_RGW_SWIFT_AUTH_H

#include "rgw_op.h"
#include "rgw_rest.h"
#include "rgw_auth.h"
#include "rgw_auth_keystone.h"
#include "rgw_auth_filters.h"

#define RGW_SWIFT_TOKEN_EXPIRATION (15 * 60)

namespace rgw {
namespace auth {
namespace swift {

/* TempURL: applier. */
class TempURLApplier : public rgw::auth::LocalApplier {
public:
  TempURLApplier(CephContext* const cct,
                 const RGWUserInfo& user_info)
    : LocalApplier(cct, user_info, LocalApplier::NO_SUBUSER) {
  };

  void modify_request_state(req_state * s) const override; /* in/out */

  struct Factory {
    virtual ~Factory() {}
    virtual aplptr_t create_apl_turl(CephContext* cct,
                                     const req_state* s,
                                     const RGWUserInfo& user_info) const = 0;
  };
};

/* TempURL: engine */
class TempURLEngine : public rgw::auth::Engine {
  using result_t = rgw::auth::Engine::result_t;

  CephContext* const cct;
  /* const */ RGWRados* const store;
  const TempURLApplier::Factory* const apl_factory;

  /* Helper methods. */
  void get_owner_info(const req_state* s,
                      RGWUserInfo& owner_info) const;
  bool is_applicable(const req_state* s) const noexcept;
  bool is_expired(const std::string& expires) const;

  class SignatureHelper;
  class PrefixableSignatureHelper;

public:
  TempURLEngine(CephContext* const cct,
                /*const*/ RGWRados* const store,
                const TempURLApplier::Factory* const apl_factory)
    : cct(cct),
      store(store),
      apl_factory(apl_factory) {
  }

  /* Interface implementations. */
  const char* get_name() const noexcept override {
    return "rgw::auth::swift::TempURLEngine";
  }

  result_t authenticate(const req_state* const s) const override;
};


/* AUTH_rgwtk */
class SignedTokenEngine : public rgw::auth::Engine {
  using result_t = rgw::auth::Engine::result_t;

  CephContext* const cct;
  RGWRados* const store;
  const rgw::auth::TokenExtractor* const extractor;
  const rgw::auth::LocalApplier::Factory* const apl_factory;

  bool is_applicable(const std::string& token) const noexcept;
  result_t authenticate(const std::string& token,
                        const req_state* s) const;

public:
  SignedTokenEngine(CephContext* const cct,
                    /* const */RGWRados* const store,
                    const rgw::auth::TokenExtractor* const extractor,
                    const rgw::auth::LocalApplier::Factory* const apl_factory)
    : cct(cct),
      store(store),
      extractor(extractor),
      apl_factory(apl_factory) {
  }

  const char* get_name() const noexcept override {
    return "rgw::auth::swift::SignedTokenEngine";
  }

  result_t authenticate(const req_state* const s) const override {
    return authenticate(extractor->get_token(s), s);
  }
};


/* External token */
class ExternalTokenEngine : public rgw::auth::Engine {
  using result_t = rgw::auth::Engine::result_t;

  CephContext* const cct;
  RGWRados* const store;
  const rgw::auth::TokenExtractor* const extractor;
  const rgw::auth::LocalApplier::Factory* const apl_factory;

  bool is_applicable(const std::string& token) const noexcept;
  result_t authenticate(const std::string& token,
                        const req_state* s) const;

public:
  ExternalTokenEngine(CephContext* const cct,
                      /* const */RGWRados* const store,
                      const rgw::auth::TokenExtractor* const extractor,
                      const rgw::auth::LocalApplier::Factory* const apl_factory)
    : cct(cct),
      store(store),
      extractor(extractor),
      apl_factory(apl_factory) {
  }

  const char* get_name() const noexcept override {
    return "rgw::auth::swift::ExternalTokenEngine";
  }

  result_t authenticate(const req_state* const s) const override {
    return authenticate(extractor->get_token(s), s);
  }
};


class SwiftAnonymousEngine : public rgw::auth::AnonymousEngine {
  const rgw::auth::TokenExtractor* const extractor;

  bool is_applicable(const req_state* s) const noexcept override {
    return extractor->get_token(s).empty();
  }

public:
  SwiftAnonymousEngine(CephContext* const cct,
                       const rgw::auth::LocalApplier::Factory* const apl_factory,
                       const rgw::auth::TokenExtractor* const extractor)
    : AnonymousEngine(cct, apl_factory),
      extractor(extractor) {
  }

  const char* get_name() const noexcept override {
    return "rgw::auth::swift::SwiftAnonymousEngine";
  }
};


class DefaultStrategy : public rgw::auth::Strategy,
                        public rgw::auth::TokenExtractor,
                        public rgw::auth::RemoteApplier::Factory,
                        public rgw::auth::LocalApplier::Factory,
                        public rgw::auth::swift::TempURLApplier::Factory {
  RGWRados* const store;

  /* The engines. */
  const rgw::auth::swift::TempURLEngine tempurl_engine;
  const rgw::auth::swift::SignedTokenEngine signed_engine;
  boost::optional <const rgw::auth::keystone::TokenEngine> keystone_engine;
  const rgw::auth::swift::ExternalTokenEngine external_engine;
  const rgw::auth::swift::SwiftAnonymousEngine anon_engine;

  using keystone_config_t = rgw::keystone::CephCtxConfig;
  using keystone_cache_t = rgw::keystone::TokenCache;
  using aplptr_t = rgw::auth::IdentityApplier::aplptr_t;
  using acl_strategy_t = rgw::auth::RemoteApplier::acl_strategy_t;

  /* The method implements TokenExtractor for X-Auth-Token present in req_state. */
  std::string get_token(const req_state* const s) const override {
    /* Returning a reference here would end in GCC complaining about a reference
     * to temporary. */
    return s->info.env->get("HTTP_X_AUTH_TOKEN", "");
  }

  aplptr_t create_apl_remote(CephContext* const cct,
                             const req_state* const s,
                             acl_strategy_t&& extra_acl_strategy,
                             const rgw::auth::RemoteApplier::AuthInfo info) const override {
    auto apl = \
      rgw::auth::add_3rdparty(store, s->account_name,
        rgw::auth::add_sysreq(cct, store, s,
          rgw::auth::RemoteApplier(cct, store, std::move(extra_acl_strategy), info,
                                   cct->_conf->rgw_keystone_implicit_tenants)));
    /* TODO(rzarzynski): replace with static_ptr. */
    return aplptr_t(new decltype(apl)(std::move(apl)));
  }

  aplptr_t create_apl_local(CephContext* const cct,
                            const req_state* const s,
                            const RGWUserInfo& user_info,
                            const std::string& subuser) const override {
    auto apl = \
      rgw::auth::add_3rdparty(store, s->account_name,
        rgw::auth::add_sysreq(cct, store, s,
          rgw::auth::LocalApplier(cct, user_info, subuser)));
    /* TODO(rzarzynski): replace with static_ptr. */
    return aplptr_t(new decltype(apl)(std::move(apl)));
  }

  aplptr_t create_apl_turl(CephContext* const cct,
                           const req_state* const s,
                           const RGWUserInfo& user_info) const override {
    /* TempURL doesn't need any user account override. It's a Swift-specific
     * mechanism that requires  account name internally, so there is no
     * business with delegating the responsibility outside. */
    return aplptr_t(new rgw::auth::swift::TempURLApplier(cct, user_info));
  }

public:
  DefaultStrategy(CephContext* const cct,
                  RGWRados* const store)
    : store(store),
      tempurl_engine(cct,
                     store,
                     static_cast<rgw::auth::swift::TempURLApplier::Factory*>(this)),
      signed_engine(cct,
                    store,
                    static_cast<rgw::auth::TokenExtractor*>(this),
                    static_cast<rgw::auth::LocalApplier::Factory*>(this)),
      external_engine(cct,
                      store,
                      static_cast<rgw::auth::TokenExtractor*>(this),
                      static_cast<rgw::auth::LocalApplier::Factory*>(this)),
      anon_engine(cct,
                  static_cast<rgw::auth::LocalApplier::Factory*>(this),
                  static_cast<rgw::auth::TokenExtractor*>(this)) {
    /* When the constructor's body is being executed, all member engines
     * should be initialized. Thus, we can safely add them. */
    using Control = rgw::auth::Strategy::Control;

    add_engine(Control::SUFFICIENT, tempurl_engine);
    add_engine(Control::SUFFICIENT, signed_engine);

    /* The auth strategy is responsible for deciding whether a parcular
     * engine is disabled or not. */
    if (! cct->_conf->rgw_keystone_url.empty()) {
      keystone_engine.emplace(cct,
                              static_cast<rgw::auth::TokenExtractor*>(this),
                              static_cast<rgw::auth::RemoteApplier::Factory*>(this),
                              keystone_config_t::get_instance(),
                              keystone_cache_t::get_instance<keystone_config_t>());

      add_engine(Control::SUFFICIENT, *keystone_engine);
    }
    if (! cct->_conf->rgw_swift_auth_url.empty()) {
      add_engine(Control::SUFFICIENT, external_engine);
    }

    add_engine(Control::SUFFICIENT, anon_engine);
  }

  const char* get_name() const noexcept override {
    return "rgw::auth::swift::DefaultStrategy";
  }
};

} /* namespace swift */
} /* namespace auth */
} /* namespace rgw */


class RGW_SWIFT_Auth_Get : public RGWOp {
public:
  RGW_SWIFT_Auth_Get() {}
  ~RGW_SWIFT_Auth_Get() override {}

  int verify_permission() override { return 0; }
  void execute() override;
  const string name() override { return "swift_auth_get"; }
};

class RGWHandler_SWIFT_Auth : public RGWHandler_REST {
public:
  RGWHandler_SWIFT_Auth() {}
  ~RGWHandler_SWIFT_Auth() override {}
  RGWOp *op_get() override;

  int init(RGWRados *store, struct req_state *state, rgw::io::BasicClient *cio) override;
  int authorize() override;
  int postauth_init() override { return 0; }
  int read_permissions(RGWOp *op) override { return 0; }

  virtual RGWAccessControlPolicy *alloc_policy() { return NULL; }
  virtual void free_policy(RGWAccessControlPolicy *policy) {}
};

class RGWRESTMgr_SWIFT_Auth : public RGWRESTMgr {
public:
  RGWRESTMgr_SWIFT_Auth() = default;
  ~RGWRESTMgr_SWIFT_Auth() override = default;

  RGWRESTMgr *get_resource_mgr(struct req_state* const s,
                               const std::string& uri,
                               std::string* const out_uri) override {
    return this;
  }

  RGWHandler_REST* get_handler(struct req_state*,
                               const rgw::auth::StrategyRegistry&,
                               const std::string&) override {
    return new RGWHandler_SWIFT_Auth;
  }
};


#endif
