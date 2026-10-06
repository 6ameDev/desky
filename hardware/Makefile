# desky hardware build helpers.
#
# Carriers live next to this file:
#   core/ — desky-core (ESP32-S3-DEVKITC-1 carrier).
#   head/ — desky-head (face-unit carrier).
# Contract and build principle: INTERFACE.md (§0 is normative).
# Guides (netlist + BOM per board) land in build/ (git-ignored).

.PHONY: check erc guides clean

check: erc
	@echo "principle: INTERFACE.md §0 (normative)"

erc:
	@echo "principle: INTERFACE.md §0 (normative)"
	kicad-cli sch erc --format json --severity-all -o build/desky-core-erc.json core/desky-core.kicad_sch
	kicad-cli sch erc --format json --severity-all -o build/desky-head-erc.json head/desky-head.kicad_sch

guides: erc
	kicad-cli sch export netlist --format kicadsexpr --output build/desky-core.net core/desky-core.kicad_sch
	kicad-cli sch export netlist --format kicadsexpr --output build/desky-head.net head/desky-head.kicad_sch
	kicad-cli sch export bom --output build/desky-core-bom.csv core/desky-core.kicad_sch
	kicad-cli sch export bom --output build/desky-head-bom.csv head/desky-head.kicad_sch

clean:
	rm -rf build
