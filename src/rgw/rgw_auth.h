// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab


#ifndef CEPH_RGW_AUTH_H
#define CEPH_RGW_AUTH_H

#include <functional>
#include <ostream>
#include <type_traits>
#include <system_error>
#include <utility>

#include "rgw_common.h"
#include "rgw_keystone.h"

#define RGW_USER_ANON_ID "anonymous"

namespace rgw {
namespace auth {

using Exception = std::system_error;


/* Load information about identity that will be used by RGWOp to authorize
 * any operation that comes from an authenticated user. */
class Identity {
public:
  typedef std::map<std::string, int> aclspec_t;
  using idset_t = boost::container::flat_set<Principal>;

  virtual ~Identity() = default;

  /* Translate the ACL provided in @aclspec into concrete permission set that
   * can be used during the authorization phase (RGWOp::verify_permission).
   * On error throws rgw::auth::Exception storing the reason.
   *
   * NOTE: an implementation is responsible for giving the real semantic to
   * the items in @aclspec. That is, their meaning may depend on particular
   * applier that is being used. */
  virtual uint32_t get_perms_from_aclspec(const aclspec_t& aclspec) const = 0;

  /* Verify whether a given identity *can be treated as* an admin of rgw_user
  * (account in Swift's terminology) specified in @uid. On error throws
  * rgw::auth::Exception storing the reason. */
  virtual bool is_admin_of(const rgw_user& uid) const = 0;

  /* Verify whether a given identity *is* the owner of the rgw_user (account
   * in the Swift's terminology) specified in @uid. On internal error throws
   * rgw::auth::Exception storing the reason. */
  virtual bool is_owner_of(const rgw_user& uid) const = 0;

  /* Return the permission mask that is used to narrow down the set of
   * operations allowed for a given identity. This method reflects the idea
   * of subuser tied to RGWUserInfo. On  error throws rgw::auth::Exception
   * with the reason. */
  virtual uint32_t get_perm_mask() const = 0;

  virtual bool is_anonymous() const final {
    /* If the identity owns the anonymous account (rgw_user), it's considered
     * the anonymous identity. On error throws rgw::auth::Exception storing
     * the reason. */
    return is_owner_of(rgw_user(RGW_USER_ANON_ID));
  }

  virtual void to_str(std::ostream& out) const = 0;

  /* Verify whether a given identity corresponds to an identity in the
     provided set */
  virtual bool is_identity(const idset_t& ids) const = 0;
};

inline std::ostream& operator<<(std::ostream& out,
                                const rgw::auth::Identity& id) {
  id.to_str(out);
  return out;
}


std::unique_ptr<Identity> transform_old_authinfo(const req_state* const s);


/* Interface for classes applying changes to request state/RADOS store
 * imposed by a particular rgw::auth::Engine.
 *
 * In contrast to rgw::auth::Engine, implementations of this interface
 * are allowed to handle req_state or RGWRados in the read-write manner.
 *
 * It's expected that most (if not all) of implementations will also
 * conform to rgw::auth::Identity interface to provide authorization
 * policy (ACLs, account's ownership and entitlement). */
class IdentityApplier : public Identity {
public:
  typedef std::unique_ptr<IdentityApplier> aplptr_t;

  virtual ~IdentityApplier() {};

  /* Fill provided RGWUserInfo with information about the account that
   * RGWOp will operate on. Errors are handled solely through exceptions.
   *
   * XXX: be aware that the "account" term refers to rgw_user. The naming
   * is legacy. */
  virtual void load_acct_info(RGWUserInfo& user_info) const = 0; /* out */

  /* Apply any changes to request state. This method will be most useful for
   * TempURL of Swift API. */
  virtual void modify_request_state(req_state* s) const {}      /* in/out */
};


/* Interface class for completing the two-step authentication process.
 * Completer provides the second step - the complete() method that should
 * be called after Engine::authenticate() but before *committing* results
 * of an RGWOp (or sending a response in the case of non-mutating ops).
 *
 * The motivation driving the interface is to address those authentication
 * schemas that require message integrity verification *without* in-memory
 * data buffering. Typical examples are AWS Auth v4 and the auth mechanism
 * of browser uploads facilities both in S3 and Swift APIs (see RGWPostObj).
 * The workflow of request from the authentication point-of-view does look
 * like following one:
 *  A. authenticate (Engine::authenticate),
 *  B. authorize (see RGWOp::verify_permissions),
 *  C. execute-prepare (init potential data modifications),
 *  D. authenticate-complete - (Completer::complete),
 *  E. execute-commit - commit the modifications from point C. */
class Completer {
public:
  /* It's expected that Completers would tend to implement many interfaces
   * and be used not only in req_state::auth::completer. Ref counting their
   * instances woild be helpful. */
  typedef std::shared_ptr<Completer> cmplptr_t;

  virtual ~Completer() = default;

  /* Complete the authentication process. Return boolean indicating whether
   * the completion succeeded. On error throws rgw::auth::Exception storing
   * the reason. */
  virtual bool complete() = 0;

  /* Apply any changes to request state. The initial use case was injecting
   * the AWSv4 filter over rgw::io::RestfulClient in req_state. */
  virtual void modify_request_state(req_state* s) = 0;     /* in/out */
};


/* Interface class for authentication backends (auth engines) in RadosGW.
 *
 * An engine is supposed only to authenticate (not authorize!) requests
 * basing on their req_state and - if access has been granted - provide
 * an upper layer with:
 *  - rgw::auth::IdentityApplier to commit all changes to the request state as
 *    well as to the RADOS store (creating an account, synchronizing
 *    user-related information with external databases and so on).
 *  - rgw::auth::Completer (optionally) to finish the authentication
 *    of the request. Typical use case is verifying message integrity
 *    in AWS Auth v4 and browser uploads (RGWPostObj).
 *
 * Both of them are supposed to be wrapped in Engine::AuthResult.
 *
 * The authentication process consists of two steps:
 *  - Engine::authenticate() which should be called before *initiating*
 *    any modifications to RADOS store that are related to an operation
 *    a client wants to perform (RGWOp::execute).
 *  - Completer::complete() supposed to be called, if completer has been
 *    returned, after the authenticate() step but before *committing*
 *    those modifications or sending a response (RGWOp::complete).
 *
 * An engine outlives both Applier and Completer. It's intended to live
 * since RadosGW's initialization and handle multiple requests till
 * a reconfiguration.
 *
 * Auth engine MUST NOT make any changes to req_state nor RADOS store.
 * This is solely an Applier's responsibility!
 *
 * Separation between authentication and global state modification has
 * been introduced because many auth engines are orthogonal to appliers
 * and thus they can be decoupled. Additional motivation is to clearly
 * distinguish all portions of code modifying data structures. */
class Engine {
public:
  virtual ~Engine() = default;

  class AuthResult {
    struct rejection_mark_t {};
    bool is_rejected = false;
    int reason = 0;

    std::pair<IdentityApplier::aplptr_t, Completer::cmplptr_t> result_pair;

    AuthResult(const int reason)
      : reason(reason) {
    }

    AuthResult(rejection_mark_t&&, const int reason)
      : is_rejected(true),
        reason(reason) {
    }

    /* Allow only the reasonable combintations - returning just Completer
     * without accompanying IdentityApplier is strictly prohibited! */
    AuthResult(IdentityApplier::aplptr_t&& applier)
      : result_pair(std::move(applier), nullptr) {
    }

    AuthResult(IdentityApplier::aplptr_t&& applier,
               Completer::cmplptr_t&& completer)
      : result_pair(std::move(applier), std::move(completer)) {
    }

  public:
    enum class Status {
      /* Engine doesn't grant the access but also doesn't reject it. */
      DENIED,

      /* Engine successfully authenticated requester. */
      GRANTED,

      /* Engine strictly indicates that a request should be rejected
       * without trying any further engine. */
      REJECTED
    };

    Status get_status() const {
      if (is_rejected) {
        return Status::REJECTED;
      } else if (! result_pair.first) {
        return Status::DENIED;
      } else {
        return Status::GRANTED;
      }
    }

    int get_reason() const {
      return reason;
    }

    IdentityApplier::aplptr_t get_applier() {
      return std::move(result_pair.first);
    }

    Completer::cmplptr_t&& get_completer() {
      return std::move(result_pair.second);
    }

    static AuthResult reject(const int reason = -EACCES) {
      return AuthResult(rejection_mark_t(), reason);
    }

    static AuthResult deny(const int reason = -EACCES) {
      return AuthResult(reason);
    }

    static AuthResult grant(IdentityApplier::aplptr_t&& applier) {
      return AuthResult(std::move(applier));
    }

    static AuthResult grant(IdentityApplier::aplptr_t&& applier,
                            Completer::cmplptr_t&& completer) {
      return AuthResult(std::move(applier), std::move(completer));
    }
  };

  using result_t = AuthResult;

  /* Get name of the auth engine. */
  virtual const char* get_name() const noexcept = 0;

  /* Throwing method for identity verification. When the check is positive
   * an implementation should return Engine::result_t containing:
   *  - a non-null pointer to an object conforming the Applier interface.
   *    Otherwise, the authentication is treated as failed.
   *  - a (potentially null) pointer to an object conforming the Completer
   *    interface.
   *
   * On error throws rgw::auth::Exception containing the reason. */
  virtual result_t authenticate(const req_state* s) const = 0;
};


/* Interface for extracting a token basing from data carried by req_state. */
class TokenExtractor {
public:
  virtual ~TokenExtractor() = default;
  virtual std::string get_token(const req_state* s) const = 0;
};


/* Abstract class for stacking sub-engines to expose them as a single
 * Engine. It is responsible for ordering its sub-engines and managing
 * fall-backs between them. Derivatee is supposed to encapsulate engine
 * instances and add them using the add_engine() method in the order it
 * wants to be tried during the call to authenticate().
 *
 * Each new Strategy should be exposed to StrategyRegistry for handling
 * the dynamic reconfiguration. */
class Strategy : public Engine {
public:
  /* Specifiers controlling what happens when an associated engine fails.
   * The names and semantic has been borrowed mostly from libpam. */
  enum class Control {
    /* Failure of an engine injected with the REQUISITE specifier aborts
     * the strategy's authentication process immediately. No other engine
     * will be tried. */
    REQUISITE,

    /* Success of an engine injected with the SUFFICIENT specifier ends
     * strategy's authentication process successfully. However, denying
     * doesn't abort it -- there will be fall-back to following engine
     * if the one that failed wasn't the last one. */
    SUFFICIENT,

    /* Like SUFFICIENT with the exception that on failure the reason code
     * is not overridden. Instead, it's taken directly from the last tried
     * non-FALLBACK engine. If there was no previous non-FALLBACK engine
     * in a Strategy, then the result_t::deny(reason = -EACCES) is used. */
    FALLBACK,
  };

  Engine::result_t authenticate(const req_state* s) const override final;

  bool is_empty() const {
    return auth_stack.empty();
  }

  static int apply(const Strategy& auth_strategy, req_state* s) noexcept;

private:
  /* Using the reference wrapper here to explicitly point out we are not
   * interested in storing nulls while preserving the dynamic polymorphism. */
  using stack_item_t = std::pair<std::reference_wrapper<const Engine>,
                                 Control>;
  std::vector<stack_item_t> auth_stack;

protected:
  void add_engine(Control ctrl_flag, const Engine& engine) noexcept;
};


/* A class aggregating the knowledge about all Strategies in RadosGW. It is
 * responsible for handling the dynamic reconfiguration on e.g. realm update.
 * The definition is in rgw/rgw_auth_registry.h,
 *
 * Each new Strategy should be exposed to it. */
class StrategyRegistry;

/* rgw::auth::RemoteApplier targets those authentication engines which don't
 * need to ask the RADOS store while performing the auth process. Instead,
 * they obtain credentials from an external source like Keystone or LDAP.
 *
 * As the authenticated user may not have an account yet, RGWRemoteAuthApplier
 * must be able to create it basing on data passed by an auth engine. Those
 * data will be used to fill RGWUserInfo structure. */
class RemoteApplier : public IdentityApplier {
public:
  class AuthInfo {
    friend class RemoteApplier;
  protected:
    const rgw_user acct_user;
    const std::string acct_name;
    const uint32_t perm_mask;
    const bool is_admin;
    const uint32_t acct_type;

  public:
    enum class acct_privilege_t {
      IS_ADMIN_ACCT,
      IS_PLAIN_ACCT
    };

    AuthInfo(const rgw_user& acct_user,
             const std::string& acct_name,
             const uint32_t perm_mask,
             const acct_privilege_t level,
             const uint32_t acct_type=TYPE_NONE)
    : acct_user(acct_user),
      acct_name(acct_name),
      perm_mask(perm_mask),
      is_admin(acct_privilege_t::IS_ADMIN_ACCT == level),
      acct_type(acct_type) {
    }
  };

  using aclspec_t = rgw::auth::Identity::aclspec_t;
  typedef std::function<uint32_t(const aclspec_t&)> acl_strategy_t;

protected:
  CephContext* const cct;

  /* Read-write is intensional here due to RGWUserInfo creation process. */
  RGWRados* const store;

  /* Supplemental strategy for extracting permissions from ACLs. Its results
   * will be combined (ORed) with a default strategy that is responsible for
   * handling backward compatibility. */
  const acl_strategy_t extra_acl_strategy;

  const AuthInfo info;
  const bool implicit_tenants;

  virtual void create_account(const rgw_user& acct_user,
                              RGWUserInfo& user_info) const;          /* out */

public:
  RemoteApplier(CephContext* const cct,
                RGWRados* const store,
                acl_strategy_t&& extra_acl_strategy,
                const AuthInfo& info,
                const bool implicit_tenants)
    : cct(cct),
      store(store),
      extra_acl_strategy(std::move(extra_acl_strategy)),
      info(info),
      implicit_tenants(implicit_tenants) {
  }

  uint32_t get_perms_from_aclspec(const aclspec_t& aclspec) const override;
  bool is_admin_of(const rgw_user& uid) const override;
  bool is_owner_of(const rgw_user& uid) const override;
  bool is_identity(const idset_t& ids) const override;

  uint32_t get_perm_mask() const override { return info.perm_mask; }
  void to_str(std::ostream& out) const override;
  void load_acct_info(RGWUserInfo& user_info) const override; /* out */

  struct Factory {
    virtual ~Factory() {}
    /* Providing r-value reference here is required intensionally. Callee is
     * thus disallowed to handle std::function in a way that could inhibit
     * the move behaviour (like forgetting about std::moving a l-value). */
    virtual aplptr_t create_apl_remote(CephContext* cct,
                                       const req_state* s,
                                       acl_strategy_t&& extra_acl_strategy,
                                       const AuthInfo info) const = 0;
  };
};


/* rgw::auth::LocalApplier targets those auth engines that base on the data
 * enclosed in the RGWUserInfo control structure. As a side effect of doing
 * the authentication process, they must have it loaded. Leveraging this is
 * a way to avoid unnecessary calls to underlying RADOS store. */
class LocalApplier : public IdentityApplier {
  using aclspec_t = rgw::auth::Identity::aclspec_t;

protected:
  const RGWUserInfo user_info;
  const std::string subuser;

  uint32_t get_perm_mask(const std::string& subuser_name,
                         const RGWUserInfo &uinfo) const;

public:
  static const std::string NO_SUBUSER;

  LocalApplier(CephContext* const cct,
               const RGWUserInfo& user_info,
               std::string subuser)
    : user_info(user_info),
      subuser(std::move(subuser)) {
  }


  uint32_t get_perms_from_aclspec(const aclspec_t& aclspec) const override;
  bool is_admin_of(const rgw_user& uid) const override;
  bool is_owner_of(const rgw_user& uid) const override;
  bool is_identity(const idset_t& ids) const override;
  uint32_t get_perm_mask() const override {
    return get_perm_mask(subuser, user_info);
  }
  void to_str(std::ostream& out) const override;
  void load_acct_info(RGWUserInfo& user_info) const override; /* out */

  struct Factory {
    virtual ~Factory() {}
    virtual aplptr_t create_apl_local(CephContext* cct,
                                      const req_state* s,
                                      const RGWUserInfo& user_info,
                                      const std::string& subuser) const = 0;
    };
};


/* The anonymous abstract engine. */
class AnonymousEngine : public Engine {
  CephContext* const cct;
  const rgw::auth::LocalApplier::Factory* const apl_factory;

public:
  AnonymousEngine(CephContext* const cct,
                  const rgw::auth::LocalApplier::Factory* const apl_factory)
    : cct(cct),
      apl_factory(apl_factory) {
  }

  const char* get_name() const noexcept override {
    return "rgw::auth::AnonymousEngine";
  }

  Engine::result_t authenticate(const req_state* s) const override final;

protected:
  virtual bool is_applicable(const req_state*) const noexcept {
    return true;
  }
};

} /* namespace auth */
} /* namespace rgw */


uint32_t rgw_perms_from_aclspec_default_strategy(
  const rgw_user& uid,
  const rgw::auth::Identity::aclspec_t& aclspec);

#endif /* CEPH_RGW_AUTH_H */
