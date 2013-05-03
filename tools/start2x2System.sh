#!/bin/bash

source `which setupDemoEnvironment.sh`

tempFile="/tmp/pmtConfig.$$"

echo "boardreader ${ARTDAQDEMO_BR_HOST[0]} ${ARTDAQDEMO_BR_PORT[0]}" >> $tempFile
echo "boardreader ${ARTDAQDEMO_BR_HOST[1]} ${ARTDAQDEMO_BR_PORT[1]}" >> $tempFile
echo "eventbuilder ${ARTDAQDEMO_EB_HOST[0]} ${ARTDAQDEMO_EB_PORT[0]}" >> $tempFile
echo "eventbuilder ${ARTDAQDEMO_EB_HOST[1]} ${ARTDAQDEMO_EB_PORT[1]}" >> $tempFile

pmt.rb -p 8080 -d $tempFile
rm $tempFile
