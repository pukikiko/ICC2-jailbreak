# the appliance packages live in this external tree. include every package makefile
# the way Buildroot itself does, so adding one is just dropping the directory in.
include $(sort $(wildcard $(BR2_EXTERNAL_LIVI_APPLIANCE_PATH)/package/*/*.mk))

