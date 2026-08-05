#!/usr/bin/env python3
"""Rasterize the normalized IPC SVG using the system librsvg installation."""

import cairo
import gi

gi.require_version("Rsvg", "2.0")
from gi.repository import Rsvg


SVG = "/home/lynnhoo/dvr-repro/results/dvr-camel-bfs-figure8/normalized_ipc.svg"
PNG = "/home/lynnhoo/dvr-repro/results/dvr-camel-bfs-figure8/normalized_ipc.png"


handle = Rsvg.Handle.new_from_file(SVG)
dimensions = handle.get_dimensions()
width = int(dimensions.width)
height = int(dimensions.height)
surface = cairo.ImageSurface(cairo.FORMAT_ARGB32, width, height)
context = cairo.Context(surface)
handle.render_cairo(context)
surface.write_to_png(PNG)
print(PNG)
