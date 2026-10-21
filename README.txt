Hardware oracle probes for image_gather4_lz with dim:SQ_RSRC_IMG_3D, AMD Radeon 610M (gfx1036), Linux, float controls in tso.py FLAGS.
Run from an AnyPS5 checkout root with this directory's files copied in:
  python3 probe.py result.json; python3 probe2.py result2.json; python3 check.py   (footprint, slice and order)
  python3 probe3.py result3.json ... python3 probe7.py result7.json; python3 checkall.py   (clamp modes, borders, 1/256 snap, wide extents, ties, tiny and denormal coordinates)
checkall.py holds the model the recompiler implements and prints matched / measured rows for each result file.
