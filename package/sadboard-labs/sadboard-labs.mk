SADBOARD_LABS_VERSION = 1
SADBOARD_LABS_SITE = $(BR2_EXTERNAL_SADBOARD_PATH)/package/sadboard-labs
SADBOARD_LABS_SITE_METHOD = local
# The project's code license has not yet been selected.
SADBOARD_LABS_LICENSE = NOASSERTION
SADBOARD_LABS_MAKE_OPTS = CC="$(TARGET_CC)" CFLAGS="$(TARGET_CFLAGS)" LDFLAGS="$(TARGET_LDFLAGS)"

# Local source synchronization can include developer-built host executables.
# Always compile from source with TARGET_CC, regardless of copied timestamps.
define SADBOARD_LABS_BUILD_CMDS
    $(TARGET_MAKE_ENV) $(MAKE) -C $(@D) $(SADBOARD_LABS_MAKE_OPTS) -B all
endef

define SADBOARD_LABS_INSTALL_TARGET_CMDS
    $(TARGET_MAKE_ENV) $(MAKE) -C $(@D) $(SADBOARD_LABS_MAKE_OPTS) DESTDIR=$(TARGET_DIR) install
endef

$(eval $(generic-package))
