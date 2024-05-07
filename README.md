# Aupera VMSS2.0 Calculators

This repo contains calculators from [VMSS2.0](https://github.com/auperatech/VMSS2.0). These calculators may be built and then loaded in a VMSS2.0 video processing pipeline to execute a particular functionality.

These calculators are already built by default in released docker containers. However, you may wish to either edit them or build new calculators based on them to implement your own functionalities.

## Reference Materials

A full list of available calculators may be [found here](https://github.com/auperatech/VMSS2.0/tree/main/docs/nodes#node-table).

For a full guide on how to create a custom calculator, [refer to this tutorial](https://github.com/auperatech/VMSS2.0/tree/main/calculators/kp_predictor#readme).

Calculators take in and produce packets. A full list of packets may be [found here](https://github.com/auperatech/VMSS2.0/tree/main/docs/nodes#packet-table). This table has links to documentation for all packet types.

## Supported Hardware

Currently, the calculators on this list are tested on the AMD Kria SOM platform (KV260 and KR260)

## Prerequisites

### Kria SOM (KV260 / KR260)
First, set up VMSS2.0 on Kria SOM by following the [setup tutorial](https://github.com/auperatech/VMSS2.0/blob/main/setup/K260_Kria_SOM/README.md). Then, while in the running docker container, clone this repository.

## Building Calculators

First, follow the necessary prerequisites. Then, while in the Kria SOM docker, navigate to this directory. You may then build a calculator by running the following:
`make clean && make && make install`

By default, this will install the calculator in `/lib` as `/lib/libaupera.<name>.calculator.<version>` (i.e. `/lib/libaupera.x86_enc.calculator.1.0.0`) and overwrite any pre-existing calculators. If you wish to restore the old calculator, the easiest way is to load a new docker container.