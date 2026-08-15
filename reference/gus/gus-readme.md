copy and paste this file on https://stackedit.io/

This is only for X-plane 12. These changes were not checked on any other version besides v12.2.0 or later of X-Plane 12.

# Step 0
0. Back up the originals one before replacing them.
1. Choose one of the internal lights (Old Halogen(_current), new halogen (_new) or LED(_LED)) and rename it to lights_inn.obj
2. Go to the airplane objects folder
3. **Back up the originals one before replacing them.**
    - Now I'm assuming you have done backup
4. Drop the _LIT textures provided here, and replace the originals
5. Drop lights_inn.obj and lights_out321_XP12.obj

# Step 1
## Plane Maker
- Select and open your a319/20/21 .acf file
- Go to Standard -> Viewpoint -> Int Lights
## Changing cockpit light colors
- change the cockpit light 1, 2 and 3 for...
1. a new halogen lighting:
    - `0.80 0.55 0.30`
2. an old halogen lighting:
    - `1.00 0.37 0.16`
3. a modern LED lighting:
    - `0.82 0.82 1.0`
### adjust cockpit light type, width and headings
set cockpit light 1 to `004.00 on vert (ft)`
1. change the first width to `50 degrees` (from 0)
2. set the size to `08.00`
3. set the pitch to `-60`
4. set the heading to `180`

- Optional
change the sizes to `09.00` and `10` for the next lights

check the image `extra-need.png` for reference

To enable the roughness and reflectance (how matte and how mirror like) of the 3d model, proceed to Step 2. If you're happy as it is right now, no action needed.

Now... for unlocking the textures information... Follow the next two steps. The final steps are only for those curious and artists.

# Step 2
Patience.

Open all the following objects in your text editor:
- cab321_0.obj**
- cargo321.obj**
- chairs321.obj**
- Copilot.obj
- DCDUs.obj
- engines.obj*
- fuselage321.obj**
- fuselage321_1.obj**
- fuselage321_2.obj**
- fuselage321_3.obj**
- gear.obj
- GlassInterior.obj
- Ipad.obj
- kitchens.obj
- knobs.obj
- neo.obj*
- panels_main.obj
- panels_overhead.obj
- panels_pedestal.obj
- pedals_details.obj
- pedals_seats.obj
- pedals_tables.obj
- SatAnt.obj
- SunShades.obj
- walls_bottom.obj
- walls_outer.obj
- walls_top.obj
- wings321L.obj**
- wings321R.obj**

I recommend to use notepad++, or VSCode. Even notepad is ok.

On the file header
```
800
OBJ

GLOBAL_cockpit_lit
TEXTURE	texture.png
TEXTURE_LIT	texture_LIT.png
TEXTURE_NORMAL	texture_NML.png

```
It will not be exactly like that, but we need to add the following after the `TEXTURE_NORMAL	texture_NML.png`.
```
NORMAL_METALNESS
GLOBAL_specular	1
ATTR_shiny_rat 1.0
```

Be sure that the `ATTR_shiny_rat 1.0` is the last before any `POINT_COUNTS, VT, IDX` line.

\* ***Carda mod***: you can check and add the directive on every obj of the carda mod to enable it too.

\*\* ***Change the 321***: change the 321 to the model that you're using. 319, 321, or 321. or all that have the same name but different type. 

# Step 3 - Translucency
Still patient? Good.
Open all the following objects in your text editor:

- knobs.obj
- SunShades.obj
- fuselage321.obj**
- fuselage321_1.obj**
- fuselage321_2.obj**
- fuselage321_3.obj**
- GlassInterior.obj
- DCDUs.obj

On the file header:
```
800
OBJ

GLOBAL_cockpit_lit
TEXTURE	texture.png
TEXTURE_LIT	texture_LIT.png
TEXTURE_NORMAL	texture_NML.png
NORMAL_METALNESS
GLOBAL_specular	1
ATTR_shiny_rat 1.0
```
just before
```
ATTR_shiny_rat 1.0
```
add
```
BLEND_GLASS
```

\*\* ***Change the 321***: change the 321 to the model that you're using. 319, 321, or 321. or all that have the same name but different type. 

Note: On the GlassInterior.obj and DCDUs.obj, I prefere to clamp the `ATTR_shiny_rat` to `0.08`

# Step 4 - Why (for nerds and livery artists)
The `NORMAL_METALNESS` directive when combined with `GLOBAL_specular 1`, and `ATTR_shiny_rat 1.0`, tells X-Plane lightin engine to treat the `alpha` channel (or the oppacity) for the roughness (how matte) a surface is, while to treat the blue channel as a map for the base reflectance (how mirror like).
When used with the `BLEND_GLASS` directive, it also tells X-Plane to look up into the `alpha` channel of the albedo (the `TEXTURE texture.png`) to control the "translucency" of the triangle where the texture is mapped. The darker the `alpha channel` (thus, more transparent on all other channels on GIMP/Photoshop), more translucent something is. Good for scratches or dirty on some glasses. Like fingerprints and cracking in the sun shade. Painting the alpha channel of the albedo texture in 8% of HSL (Hex #141414), you can apply the X-Plane 12 transparency limit when using `BLEND_GLASS`.

The emisse layer (_LIT) is preferred to be used in large areas where the night light "realism" can be "baked in" and not illuminated by a true light source, having a texture that will be used in a "constant" brightness when in "night" conditions.

Any user using the setup provided here will be able to enjoy all these small details.

Feel free to put this mod into your textures. Just remember to put my name, but, tbh, I don't care much about it.

When exporting your work from substance, any customized normal map will now be on the correct (or as good as it can be) mode for the X-Plane 12 lighting.


But wait. These are all the X-Plane 11 properties! Why it works on X-Plane 12.
It's the nature of backward compatibility of the sim. Getting it right as "made to xplane 12" would require an entire new export with the new worflow. The current workflow provides support to X-Plane 12 through the X-Plane 11 rendering translation.