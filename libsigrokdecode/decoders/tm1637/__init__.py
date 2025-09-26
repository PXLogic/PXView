

"""

DECODER:
This decoder stacks on top of the ``tmc`` protocol decoder and decodes
the display driver TM1637 controlling up to 6 pcs of 7-segment digital tubes
with decimal points or one or two colons.
The decode is compatible with TM1636 driver. However, it supports just 4 tubes
and does not have the keyboard scanning interface implemented.

NOTE:
The decoder has been tested on display modules with following configurations:
- 4 digital tubes with active decimal points for each tube without active colon
- 4 digital tubes with active colon with inactive decimal points

Althoug the driver TM1637 has keyboard scanning capability, it is not utilized
on display modules, which have no switches mounted.

"""

from .pd import Decoder
