// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "acconfig.h"
#include "tools/rbd/ArgumentTypes.h"
#include "tools/rbd/Shell.h"
#include "tools/rbd/Utils.h"
#include "include/krbd.h"
#include "include/stringify.h"
#include "include/uuid.h"
#include "common/config.h"
#include "common/errno.h"
#include "common/safe_io.h"
#include "common/strtol.h"
#include "common/Formatter.h"
#include "msg/msg_types.h"
#include "global/global_context.h"
#include <iostream>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/scope_exit.hpp>
#include <boost/program_options.hpp>

namespace rbd {
namespace action {
namespace kernel {

namespace at = argument_types;
namespace po = boost::program_options;

namespace {

std::map<std::string, std::string> map_options; // used for both map and unmap

} // anonymous namespace

static std::string map_option_uuid_cb(const char *value_char)
{
  uuid_d u;
  if (!u.parse(value_char))
    return "";

  return stringify(u);
}

static std::string map_option_ip_cb(const char *value_char)
{
  entity_addr_t a;
  const char *endptr;
  if (!a.parse(value_char, &endptr) ||
      endptr != value_char + strlen(value_char)) {
    return "";
  }

  return stringify(a.get_sockaddr());
}

static std::string map_option_int_cb(const char *value_char)
{
  std::string err;
  int d = strict_strtol(value_char, 10, &err);
  if (!err.empty() || d < 0)
    return "";

  return stringify(d);
}

static void put_map_option(const std::string &key, std::string val)
{
  map_options[key] = val;
}

static int put_map_option_value(const std::string &opt, const char *value_char,
                                std::string (*parse_cb)(const char *))
{
  if (!value_char || *value_char == '\0') {
    std::cerr << "rbd: " << opt << " option requires a value" << std::endl;
    return -EINVAL;
  }

  std::string value = parse_cb(value_char);
  if (value.empty()) {
    std::cerr << "rbd: invalid " << opt << " value '" << value_char << "'"
              << std::endl;
    return -EINVAL;
  }

  put_map_option(opt, opt + "=" + value);
  return 0;
}

static int parse_map_options(char *options)
{
  for (char *this_char = strtok(options, ", ");
       this_char != NULL;
       this_char = strtok(NULL, ",")) {
    char *value_char;

    if ((value_char = strchr(this_char, '=')) != NULL)
      *value_char++ = '\0';

    if (!strcmp(this_char, "fsid")) {
      if (put_map_option_value("fsid", value_char, map_option_uuid_cb))
        return -EINVAL;
    } else if (!strcmp(this_char, "ip")) {
      if (put_map_option_value("ip", value_char, map_option_ip_cb))
        return -EINVAL;
    } else if (!strcmp(this_char, "share") || !strcmp(this_char, "noshare")) {
      put_map_option("share", this_char);
    } else if (!strcmp(this_char, "crc") || !strcmp(this_char, "nocrc")) {
      put_map_option("crc", this_char);
    } else if (!strcmp(this_char, "cephx_require_signatures") ||
               !strcmp(this_char, "nocephx_require_signatures")) {
      put_map_option("cephx_require_signatures", this_char);
    } else if (!strcmp(this_char, "tcp_nodelay") ||
               !strcmp(this_char, "notcp_nodelay")) {
      put_map_option("tcp_nodelay", this_char);
    } else if (!strcmp(this_char, "cephx_sign_messages") ||
               !strcmp(this_char, "nocephx_sign_messages")) {
      put_map_option("cephx_sign_messages", this_char);
    } else if (!strcmp(this_char, "mount_timeout")) {
      if (put_map_option_value("mount_timeout", value_char, map_option_int_cb))
        return -EINVAL;
    } else if (!strcmp(this_char, "osdkeepalive")) {
      if (put_map_option_value("osdkeepalive", value_char, map_option_int_cb))
        return -EINVAL;
    } else if (!strcmp(this_char, "osd_idle_ttl")) {
      if (put_map_option_value("osd_idle_ttl", value_char, map_option_int_cb))
        return -EINVAL;
    } else if (!strcmp(this_char, "rw") || !strcmp(this_char, "ro")) {
      put_map_option("rw", this_char);
    } else if (!strcmp(this_char, "queue_depth")) {
      if (put_map_option_value("queue_depth", value_char, map_option_int_cb))
        return -EINVAL;
    } else if (!strcmp(this_char, "lock_on_read")) {
      put_map_option("lock_on_read", this_char);
    } else if (!strcmp(this_char, "exclusive")) {
      put_map_option("exclusive", this_char);
    } else {
      std::cerr << "rbd: unknown map option '" << this_char << "'" << std::endl;
      return -EINVAL;
    }
  }

  return 0;
}

static int parse_unmap_options(char *options)
{
  for (char *this_char = strtok(options, ", ");
       this_char != NULL;
       this_char = strtok(NULL, ",")) {
    char *value_char;

    if ((value_char = strchr(this_char, '=')) != NULL)
      *value_char++ = '\0';

    if (!strcmp(this_char, "force")) {
      put_map_option("force", this_char);
    } else {
      std::cerr << "rbd: unknown unmap option '" << this_char << "'" << std::endl;
      return -EINVAL;
    }
  }

  return 0;
}

static int do_kernel_showmapped(Formatter *f)
{
#if defined(WITH_KRBD)
  struct krbd_ctx *krbd;
  int r;

  r = krbd_create_from_context(g_ceph_context, &krbd);
  if (r < 0)
    return r;

  r = krbd_showmapped(krbd, f);

  krbd_destroy(krbd);
  return r;
#else
  return -1;
#endif

}

static int get_unsupported_features(librbd::Image &image,
                                    uint64_t *unsupported_features)
{
  char buf[20];
  uint64_t features, supported_features;
  int r;

  r = safe_read_file("/sys/bus/rbd/", "supported_features", buf,
                     sizeof(buf) - 1);
  if (r < 0)
    return r;

  buf[r] = '\0';
  try {
    supported_features = std::stoull(buf, nullptr, 16);
  } catch (...) {
    return -EINVAL;
  }

  r = image.features(&features);
  if (r < 0)
    return r;

  *unsupported_features = features & ~supported_features;
  return 0;
}

/*
 * hint user to check syslog for krbd related messages and provide suggestions
 * based on errno return by krbd_map(). also note that even if some librbd calls
 * fail, we at least dump the "try dmesg..." message to aid debugging.
 */
static void print_error_description(const char *poolname, const char *imgname,
				    const char *snapname, int maperrno)
{
  int r;
  uint8_t oldformat;
  librados::Rados rados;
  librados::IoCtx ioctx;
  librbd::Image image;

  if (maperrno == -ENOENT)
    goto done;

  r = utils::init_and_open_image(poolname, imgname, "", snapname,
				 true, &rados, &ioctx, &image);
  if (r < 0)
    goto done;

  r = image.old_format(&oldformat);
  if (r < 0)
    goto done;

  /*
   * kernel returns -ENXIO when mapping a V2 image due to unsupported feature
   * set - so, hint about that too...
   */
  if (!oldformat && (maperrno == -ENXIO)) {
    uint64_t unsupported_features;
    bool need_terminate = true;

    std::cout << "RBD image feature set mismatch. ";
    r = get_unsupported_features(image, &unsupported_features);
    if (r == 0 && (unsupported_features & ~RBD_FEATURES_ALL) == 0) {
      uint64_t immutable = RBD_FEATURES_ALL & ~(RBD_FEATURES_MUTABLE |
                                                RBD_FEATURES_DISABLE_ONLY);
      if (unsupported_features & immutable) {
        std::cout << "This image cannot be mapped because the following "
                  << "immutable features are unsupported by the kernel:";
        unsupported_features &= immutable;
        need_terminate = false;
      } else {
        std::cout << "You can disable features unsupported by the kernel "
                  << "with \"rbd feature disable ";

        if (poolname != utils::get_default_pool_name()) {
          std::cout << poolname << "/";
        }
        std::cout << imgname;
      }
    } else {
      std::cout << "Try disabling features unsupported by the kernel "
                << "with \"rbd feature disable";
      unsupported_features = 0;
    }
    for (auto it : at::ImageFeatures::FEATURE_MAPPING) {
      if (it.first & unsupported_features) {
        std::cout << " " << it.second;
      }
    }
    if (need_terminate)
      std::cout << "\"";
    std::cout << "." << std::endl;
  }

 done:
  std::cout << "In some cases useful info is found in syslog - try \"dmesg | tail\"." << std::endl;
}

static int do_kernel_map(const char *poolname, const char *imgname,
                         const char *snapname)
{
#if defined(WITH_KRBD)
  struct krbd_ctx *krbd;
  std::ostringstream oss;
  char *devnode;
  int r;

  r = krbd_create_from_context(g_ceph_context, &krbd);
  if (r < 0)
    return r;

  for (std::map<std::string, std::string>::iterator it = map_options.begin();
       it != map_options.end(); ) {
    // for compatibility with < 3.7 kernels, assume that rw is on by
    // default and omit it even if it was specified by the user
    // (see ceph.git commit fb0f1986449b)
    if (it->first == "rw" && it->second == "rw") {
      it = map_options.erase(it);
    } else {
      if (it != map_options.begin())
        oss << ",";
      oss << it->second;
      ++it;
    }
  }

  r = krbd_map(krbd, poolname, imgname, snapname, oss.str().c_str(), &devnode);
  if (r < 0) {
    print_error_description(poolname, imgname, snapname, r);
    goto out;
  }

  std::cout << devnode << std::endl;

  free(devnode);
out:
  krbd_destroy(krbd);
  return r;
#else
  return -1;
#endif
}

static int do_kernel_unmap(const char *dev, const char *poolname,
                           const char *imgname, const char *snapname)
{
#if defined(WITH_KRBD)
  struct krbd_ctx *krbd;
  std::ostringstream oss;
  int r;

  r = krbd_create_from_context(g_ceph_context, &krbd);
  if (r < 0)
    return r;

  for (auto it = map_options.cbegin(); it != map_options.cend(); ++it) {
    if (it != map_options.cbegin())
      oss << ",";
    oss << it->second;
  }

  if (dev)
    r = krbd_unmap(krbd, dev, oss.str().c_str());
  else
    r = krbd_unmap_by_spec(krbd, poolname, imgname, snapname,
                           oss.str().c_str());

  krbd_destroy(krbd);
  return r;
#else
  return -1;
#endif

}

void get_show_arguments(po::options_description *positional,
                        po::options_description *options) {
  at::add_format_options(options);
}

int execute_show(const po::variables_map &vm) {
  at::Format::Formatter formatter;
  int r = utils::get_formatter(vm, &formatter);
  if (r < 0) {
    return r;
  }

  utils::init_context();

  r = do_kernel_showmapped(formatter.get());
  if (r < 0) {
    std::cerr << "rbd: showmapped failed: " << cpp_strerror(r) << std::endl;
    return r;
  }
  return 0;
}

void get_map_arguments(po::options_description *positional,
                       po::options_description *options) {
  at::add_image_or_snap_spec_options(positional, options,
                                     at::ARGUMENT_MODIFIER_NONE);
  options->add_options()
    ("options,o", po::value<std::string>(), "map options")
    ("read-only", po::bool_switch(), "map read-only")
    ("exclusive", po::bool_switch(), "disable automatic exclusive lock transitions");
}

int execute_map(const po::variables_map &vm) {
  size_t arg_index = 0;
  std::string pool_name;
  std::string image_name;
  std::string snap_name;
  int r = utils::get_pool_image_snapshot_names(
    vm, at::ARGUMENT_MODIFIER_NONE, &arg_index, &pool_name, &image_name,
    &snap_name, utils::SNAPSHOT_PRESENCE_PERMITTED,
    utils::SPEC_VALIDATION_NONE);
  if (r < 0) {
    return r;
  }

  if (vm["read-only"].as<bool>()) {
    put_map_option("rw", "ro");
  }
  if (vm["exclusive"].as<bool>()) {
    put_map_option("exclusive", "exclusive");
  }

  // parse default options first so they can be overwritten by cli options
  char *default_map_options = strdup(g_conf->get_val<std::string>(
    "rbd_default_map_options").c_str());
  BOOST_SCOPE_EXIT( (default_map_options) ) {
    free(default_map_options);
  } BOOST_SCOPE_EXIT_END;

  if (parse_map_options(default_map_options)) {
    std::cerr << "rbd: couldn't parse default map options" << std::endl;
    return -EINVAL;
  }

  if (vm.count("options")) {
    char *cli_map_options = strdup(vm["options"].as<std::string>().c_str());
    BOOST_SCOPE_EXIT( (cli_map_options) ) {
      free(cli_map_options);
    } BOOST_SCOPE_EXIT_END;

    if (parse_map_options(cli_map_options)) {
      std::cerr << "rbd: couldn't parse map options" << std::endl;
      return -EINVAL;
    }
  }

  utils::init_context();

  r = do_kernel_map(pool_name.c_str(), image_name.c_str(), snap_name.c_str());
  if (r < 0) {
    std::cerr << "rbd: map failed: " << cpp_strerror(r) << std::endl;
    return r;
  }

  return 0;
}

void get_unmap_arguments(po::options_description *positional,
                   po::options_description *options) {
  positional->add_options()
    ("image-or-snap-or-device-spec",
     "image, snapshot, or device specification\n"
     "[<pool-name>/]<image-name>[@<snapshot-name>] or <device-path>");
  at::add_pool_option(options, at::ARGUMENT_MODIFIER_NONE);
  at::add_image_option(options, at::ARGUMENT_MODIFIER_NONE);
  at::add_snap_option(options, at::ARGUMENT_MODIFIER_NONE);
  options->add_options()
    ("options,o", po::value<std::string>(), "unmap options");
}

int execute_unmap(const po::variables_map &vm) {
  std::string device_name = utils::get_positional_argument(vm, 0);
  if (!boost::starts_with(device_name, "/dev/")) {
    device_name.clear();
  }

  size_t arg_index = 0;
  std::string pool_name;
  std::string image_name;
  std::string snap_name;
  int r;
  if (device_name.empty()) {
    r = utils::get_pool_image_snapshot_names(
      vm, at::ARGUMENT_MODIFIER_NONE, &arg_index, &pool_name, &image_name,
      &snap_name, utils::SNAPSHOT_PRESENCE_PERMITTED,
      utils::SPEC_VALIDATION_NONE, false);
    if (r < 0) {
      return r;
    }
  }

  if (device_name.empty() && image_name.empty()) {
    std::cerr << "rbd: unmap requires either image name or device path"
              << std::endl;
    return -EINVAL;
  }

  if (vm.count("options")) {
    char *cli_unmap_options = strdup(vm["options"].as<std::string>().c_str());
    BOOST_SCOPE_EXIT( (cli_unmap_options) ) {
      free(cli_unmap_options);
    } BOOST_SCOPE_EXIT_END;

    if (parse_unmap_options(cli_unmap_options)) {
      std::cerr << "rbd: couldn't parse unmap options" << std::endl;
      return -EINVAL;
    }
  }

  utils::init_context();

  r = do_kernel_unmap(device_name.empty() ? nullptr : device_name.c_str(),
                      pool_name.c_str(), image_name.c_str(),
                      snap_name.empty() ? nullptr : snap_name.c_str());
  if (r < 0) {
    std::cerr << "rbd: unmap failed: " << cpp_strerror(r) << std::endl;
    return r;
  }
  return 0;
}

Shell::SwitchArguments switched_arguments({"read-only", "exclusive"});
Shell::Action action_show(
  {"showmapped"}, {}, "Show the rbd images mapped by the kernel.", "",
  &get_show_arguments, &execute_show);

Shell::Action action_map(
  {"map"}, {}, "Map image to a block device using the kernel.", "",
  &get_map_arguments, &execute_map);

Shell::Action action_unmap(
  {"unmap"}, {}, "Unmap a rbd device that was used by the kernel.", "",
  &get_unmap_arguments, &execute_unmap);

} // namespace kernel
} // namespace action
} // namespace rbd
