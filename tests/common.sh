[ -n "$VERBOSE" ] && set -x
top_builddir=`cd ${top_builddir-.}; pwd`
PRINTERD="${top_builddir}/src/printerd"
PDCLI="${top_builddir}/tools/pd-cli"
PRINTERD_SESSION_BUS="${top_builddir}"/printerd-session.bus
if [ -r "${PRINTERD_SESSION_BUS}" ]; then
  DBUS_SESSION_BUS_ADDRESS="$(cat "${PRINTERD_SESSION_BUS}")"
  export DBUS_SESSION_BUS_ADDRESS
fi
unset PRINTERD_SESSION_BUS
PD_DEST=org.freedesktop.printerd
PD_PATH=/org/freedesktop/printerd
PD_IFACE=org.freedesktop.printerd

BOOKMARK="${BOOKMARK-"${top_builddir}/printerd-session.lines-before-test"}"
SESSION_LOG="${SESSION_LOG-"${top_builddir}"/printerd-session.log}"
# Call this function to report test results
result_is () {
  n=$(cat "${BOOKMARK}")
  if [ "$1" -eq 0 ]; then
    if grep -q '\*\*' \
      <(sed -e "1,${n}d" "${SESSION_LOG}"); then
      printf "Warnings/errors in output:\n"
      sed -e "1,${n}d" "${SESSION_LOG}"
      exit 1
    fi
  else
    printf "printerd output:\n"
    sed -e "1,${n}d" "${SESSION_LOG}"
  fi
  exit "$1"
}

# Remember where we're up to in printerd's output
wc -l < "${SESSION_LOG}" > "${BOOKMARK}"

simple_ppd () {
    tmp="$(mktemp /tmp/printerd.XXXXXXXXXX)"
    # Create a simple PPD.
    cat >"$tmp" <<"EOF"
*PPD-Adobe: "4.3"
*FormatVersion:	"4.3"
*FileVersion:	"1.1"
*LanguageVersion: English
*LanguageEncoding: ISOLatin1
*PCFileName:	"CUPSFILTER.PPD"
*Manufacturer:	"Generic"
*Product:	"(Generic Printer)"
*cupsFilter:    "application/vnd.cups-raster 0 -"
*ModelName:     "Generic Printer"
*ShortNickName: "Generic Printer"
*NickName:      "Generic Printer"
*PSVersion:	"(2017.000) 0"
*LanguageLevel:	"2"
*ColorDevice:	True
*DefaultColorSpace: RGB
*FileSystem:	False
*Throughput:	"1"
*LandscapeOrientation: Plus90
*VariablePaperSize: False
*TTRasterizer:	Type42
*HWMargins: "9 9 9 9"

*OpenUI *PageSize/Page Size: PickOne
*OrderDependency: 10 AnySetup *PageSize
*DefaultPageSize: Letter
*PageSize Letter/Letter:  "<</cupsPageSizeName (1) /PageSize[612 792]/ImagingBBox null>>setpagedevice"
*PageSize Legal/Legal:    "<</cupsPageSizeName (3) /PageSize[612 1008]/ImagingBBox null>>setpagedevice"
*PageSize A4/A4:          "<</cupsPageSizeName (2) /PageSize[595 842]/ImagingBBox null>>setpagedevice"
*CloseUI: *PageSize

*OpenUI *PageRegion/Page Size: PickOne
*OrderDependency: 10 AnySetup *PageRegion
*DefaultPageRegion: Letter
*PageRegion Letter/Letter:  "<</cupsPageSizeName (1) /PageSize[612 792]/ImagingBBox null>>setpagedevice"
*PageRegion A4/A4:          "<</cupsPageSizeName (2) /PageSize[595 842]/ImagingBBox null>>setpagedevice"
*PageRegion Legal/Legal:    "<</cupsPageSizeName (3) /PageSize[612 1008]/ImagingBBox null>>setpagedevice"
*CloseUI: *PageRegion

*DefaultImageableArea: Letter
*ImageableArea Letter/US Letter:    "9 9 594 756"
*ImageableArea A4/A4:               "9 9 586.28 833"
*ImageableArea Legal/Legal:         "9 9 594 999"

*DefaultPaperDimension: Letter
*PaperDimension Letter/US Letter:   "612 792"
*PaperDimension A4/A4:              "595 842"
*PaperDimension Legal/Legal:        "612 1008"
EOF
    printf "%s" "$tmp"
}

sample_pdf () {
    tmp="$(mktemp /tmp/printerd.XXXXXXXXXX)"
    # Create a PDF to print.
    # This comes from: /usr/lib/cups/filter/texttopdf 1 test '' 1 ''
    # from cups-filters-1.0.58
    cat >"$tmp" <<"EOF"
%PDF-1.3
%cupsRotation: 0
3 0 obj
<</Type/Font
  /Subtype /Type1
  /BaseFont /Courier-Bold
>>
endobj
4 0 obj
<</Type/Font
  /Subtype /Type1
  /BaseFont /Courier
>>
endobj
2 0 obj
<<
  /FB00 3 0 R
  /FN00 4 0 R
>>
endobj
1 0 obj
<</Type/Pages
  /Count 0
  /Kids []
>>
endobj
5 0 obj
<</Type/Catalog
  /Pages 1 0 R
>>
endobj
6 0 obj
<<
  /Creator (texttopdf/1.0.58)
  /CreationDate (D:20141121162117+00'00')
  /Title ()
  /Author (test)
>>
endobj
xref
0 7
0000000000 65535 f 
0000000220 00000 n 
0000000171 00000 n 
0000000026 00000 n 
0000000101 00000 n 
0000000274 00000 n 
0000000323 00000 n 
trailer
<<
  /Size 7
  /Root 5 0 R
  /Info 6 0 R
>>
startxref
445
%%EOF
EOF
    printf "%s" "$tmp"
}