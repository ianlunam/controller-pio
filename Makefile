# Thin wrapper over PlatformIO for the lounge display controller.
# Everything here is a one-liner you could type by hand; the point is that the
# flags for OTA, the native test env and the secrets check are easy to forget.
#
# Run `make help` for the target list.

PIO         ?= pio
FIRMWARE_ENV ?= esp32dev
OTA_ENV      ?= ota
# Honour SECRETS_FILE from the environment, since scripts/secret.sh reads it,
# and re-export so `make SECRETS=...` reaches the script too.
SECRETS      ?= $(if $(SECRETS_FILE),$(SECRETS_FILE),../.secrets)
export SECRETS_FILE := $(SECRETS)
SECRET_KEYS  := WIFI_SSID WIFI_PWD TIMEZONE MQTT_BROKER MQTT_PORT MQTT_USER MQTT_PWD

.DEFAULT_GOAL := build

.PHONY: help build upload ota monitor upload-monitor test check size \
        compiledb clean fullclean erase check-secrets

help:
	@echo "Building"
	@echo "  build           compile the firmware (default)"
	@echo "  size            compile and report flash/RAM usage"
	@echo "  compiledb       regenerate compile_commands.json for the editor"
	@echo ""
	@echo "Flashing"
	@echo "  upload          flash over USB"
	@echo "  ota             flash over the air (see OTA target below)"
	@echo "  upload-monitor  flash over USB, then open the serial monitor"
	@echo "  monitor         open the serial monitor"
	@echo "  erase           erase the whole flash (destructive)"
	@echo ""
	@echo "Checking"
	@echo "  test            run the host unit tests (lib/BinSchedule, lib/MqttPayload)"
	@echo "  check           run static analysis over src/ and lib/"
	@echo "  check-secrets   report which keys $(SECRETS) is missing (prints no values)"
	@echo ""
	@echo "Cleaning"
	@echo "  clean           remove build output for $(FIRMWARE_ENV)"
	@echo "  fullclean       also remove downloaded platforms and libraries"
	@echo ""
	@echo "Overridable: PIO=$(PIO) FIRMWARE_ENV=$(FIRMWARE_ENV) OTA_ENV=$(OTA_ENV) SECRETS=$(SECRETS)"
	@echo "OTA target is upload_port in the [env:$(OTA_ENV)] section of platformio.ini"

# --- building --------------------------------------------------------------

build: check-secrets
	$(PIO) run -e $(FIRMWARE_ENV)

size: check-secrets
	$(PIO) run -e $(FIRMWARE_ENV) -t size

compiledb:
	$(PIO) run -e $(FIRMWARE_ENV) -t compiledb

# --- flashing --------------------------------------------------------------

upload: check-secrets
	$(PIO) run -e $(FIRMWARE_ENV) -t upload

# Needs a board already running firmware that calls ArduinoOTA.handle(), so the
# first flash after a long gap has to be `make upload` over USB.
ota: check-secrets
	$(PIO) run -e $(OTA_ENV) -t upload

monitor:
	$(PIO) device monitor

upload-monitor: upload monitor

erase:
	$(PIO) run -e $(FIRMWARE_ENV) -t erase

# --- checking --------------------------------------------------------------

# Host-side only: the pure logic in lib/. Nothing in src/ is covered, because
# it all touches the display, touch panel or network.
test:
	$(PIO) test -e native

check:
	$(PIO) check -e $(FIRMWARE_ENV)

# Reports presence, never values, so it is safe to run with someone watching.
check-secrets:
	@missing=""; \
	for key in $(SECRET_KEYS); do \
		if bash scripts/secret.sh $$key >/dev/null 2>&1; then \
			printf '  ok       %s\n' "$$key"; \
		else \
			printf '  MISSING  %s\n' "$$key"; \
			missing="$$missing $$key"; \
		fi; \
	done; \
	if [ -n "$$missing" ]; then \
		printf '\nAdd to %s, one "KEY value" pair per line:%s\n' "$(SECRETS)" "$$missing" >&2; \
		exit 1; \
	fi

# --- cleaning --------------------------------------------------------------

clean:
	$(PIO) run -e $(FIRMWARE_ENV) -t clean

fullclean:
	$(PIO) run -e $(FIRMWARE_ENV) -t fullclean
