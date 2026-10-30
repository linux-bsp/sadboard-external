# Include SadBoard package recipes when they are added.
include $(sort $(wildcard $(BR2_EXTERNAL_SADBOARD_PATH)/package/*/*.mk))
