SADBOARD_LABS_VERSION = 1
SADBOARD_LABS_SITE = $(BR2_EXTERNAL_SADBOARD_PATH)/package/sadboard-labs
SADBOARD_LABS_SITE_METHOD = local
# The project's code license has not yet been selected.
SADBOARD_LABS_LICENSE = NOASSERTION

define SADBOARD_LABS_BUILD_CMDS
    $(TARGET_MAKE_ENV) $(MAKE) -C $(@D) CC="$(TARGET_CC)" \
        CFLAGS="$(TARGET_CFLAGS)" LDFLAGS="$(TARGET_LDFLAGS)"
endef

define SADBOARD_LABS_INSTALL_TARGET_CMDS
    $(INSTALL) -D -m 0755 $(@D)/crash-bug $(TARGET_DIR)/usr/libexec/sadboard/crash-bug
    $(INSTALL) -D -m 0755 $(@D)/crash-fixed $(TARGET_DIR)/usr/libexec/sadboard/crash-fixed
    $(INSTALL) -D -m 0755 $(@D)/sadboard $(TARGET_DIR)/usr/bin/sadboard
    $(INSTALL) -D -m 0644 $(@D)/src/crash.c $(TARGET_DIR)/usr/src/sadboard/src/crash.c
endef

$(eval $(generic-package))
