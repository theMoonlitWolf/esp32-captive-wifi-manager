# Changelog

All notable changes to the ESP32 Captive WiFi Manager project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased] - v0.2.0

### Added

- AP-only mode support for devices that don't need STA connection or in cases where no other (WPA2/FREE) AP is available
- Improved captive portal logic and user experience
- Better error handling and recovery mechanisms

### Fixed

- Multiple bug fixes improving stability
- Enhanced reconnection logic
- Improved state management during mode transitions

### Changed

- Refactored WiFi state machine for better reliability
- Enhanced DNS server performance for captive portal
- Optimized memory usage

### In Progress

- Documentation improvements

## [0.1.1] - 2025-11-01

### Added
- **Full example**: Comprehensive example application showcasing all features including:
  - WebSocket communication (binary and JSON protocols)
  - Custom HTTP endpoints
  - Form handling
  - LED control
  - System status monitoring

### Fixed
- **Captive portal SSID selection**: Dropdown now correctly selects the saved WiFi network if it is found in the scan results
- **Authentication mode handling**: Authmode setting is now correctly passed to and from the captive portal frontend
- Improved form data processing and validation

### Changed
- Enhanced captive portal UI for better user experience
- Improved saved network detection logic

## [0.1.0] - 2025-10-31

### Added
- **Initial Release**: First working version after migration from template project to standalone ESP-IDF component
- Core WiFi management functionality:
  - Automatic WiFi connection on boot
  - Captive portal for WiFi configuration
  - NVS-based credential persistence
  - WiFi scanning and network selection
- Network features:
  - Static IP configuration
  - DHCP support
  - mDNS hostname configuration
  - Service discovery via mDNS
- SD card integration:
  - File serving capability
  - Configurable SPI pins
  - Optional auto-format on mount failure
- Status indication:
  - SK6812 LED support
  - Multiple blink patterns for different states
  - Configurable GPIO pin
- HTTP server:
  - Built-in captive portal interface
  - Custom handler registration (up to 8 handlers)
  - WebSocket support
- Configuration system:
  - Extensive Kconfig options
  - Configurable via ESP-IDF menuconfig
  - Pin configuration
  - Behavior settings
  - Logging levels
- Component structure:
  - Proper ESP-IDF component layout
  - ESP Component Manager support
  - Clean include/src organization
  - Embedded HTML for captive portal

### Changed
- Migrated from standalone project to reusable ESP-IDF component
- Restructured code for better modularity
- Improved documentation and code comments

### Technical Details
- Minimum ESP-IDF version: 5.5.0
- Dependencies:
  - `espressif/mdns`: ^1.8.2
  - `espressif/led_indicator`: ^1.1.1
  - `dns_server`, not available on the component registry
- Component registration with proper dependencies
- Build system integration with CMakeLists.txt

---

## Version History Summary

| Version | Date       | Key Changes |
|---------|------------|-------------|
| 0.1.0   | 2025-10-31 | Initial component release with core WiFi management |
| 0.1.1   | 2025-11-01 | Bug fixes, improved captive portal, added full example |
| 0.2.0   | TBD        | AP-only mode, enhanced stability, bug fixes |

---

## Migration Notes

### From v0.1.0 to v0.1.1
- No breaking changes
- Simply update component version in `idf_component.yml`
- Existing configurations and NVS data remain compatible

### From v0.1.1 to v0.2.0
- No breaking API changes expected
- Enhanced mode handling may improve connection reliability
- Review Kconfig options for new settings

---

## Future Roadmap

- [ ] Reusable LED indicator in the rest of the project
- [ ] Multiple WiFi credential storage
- [ ] Web-based OTA updates
- [ ] Enhanced security features
- [ ] Power management optimizations
- [ ] Additional authentication methods (WPA3)

---

## Links

- [GitHub Repository](https://github.com/theMoonlitWolf/esp32-captive-wifi-manager)
- [ESP Component Registry](https://components.espressif.com/)
- [Release Notes](https://github.com/theMoonlitWolf/esp32-captive-wifi-manager/releases)
