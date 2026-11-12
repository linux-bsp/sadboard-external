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

# The disposable storage laboratory fixture is built by native host tools.
SADBOARD_LABS_DEPENDENCIES += host-e2fsprogs

define SADBOARD_LABS_FIXTURE
    mkdir -p $(@D)/fixtures
    rm -f $(@D)/fixtures/storage.ext4
    truncate -s 16M $(@D)/fixtures/storage.ext4
    E2FSPROGS_FAKE_TIME=1700000000 $(HOST_DIR)/sbin/mke2fs -q -F -t ext4 -b 4096 -m 0 \
        -U 04200000-0000-4000-8000-000000000001 -O ^has_journal \
        -E lazy_itable_init=0,lazy_journal_init=0,hash_seed=04200000-0000-4000-8000-000000000001 \
        $(@D)/fixtures/storage.ext4
endef
SADBOARD_LABS_POST_BUILD_HOOKS += SADBOARD_LABS_FIXTURE

define SADBOARD_LABS_INSTALL_FIXTURE
    $(INSTALL) -D -m 0644 $(@D)/fixtures/storage.ext4 $(TARGET_DIR)/usr/libexec/sadboard/fixtures/storage.ext4
endef
SADBOARD_LABS_POST_INSTALL_TARGET_HOOKS += SADBOARD_LABS_INSTALL_FIXTURE

SADBOARD_LABS_MODULE_SUBDIRS = kernel
$(eval $(kernel-module))
$(eval $(generic-package))
