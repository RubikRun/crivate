# Class used to apply permutations to pixels of an image
class ImagePermuter:
    # Returns the color at a given pixel position in an image with applied permutations.
    # The pixels of the image are separated into regions at multiple levels recursively.
    # The number of those regions horizontally and vertically for all levels
    # should be provided in the rgn_schemes parameter.
    # Those regions are then permuted/shuffled with the given permutations.
    # The function returns the color inside that shuffled image.
    def get_color_at(pos_pixel, image, rgn_perms, rgn_schemes):
        # Get the resolution of the image
        image_resolution = [image.get_width(), image.get_height()]
        # Permute the pixel position to get the position of the pixel after applying the permutations
        pos_perm_pixel = ImagePermuter.permute_pixel_position(pos_pixel, image_resolution, rgn_perms, rgn_schemes)
        # Return the image color of the permuted pixel
        return image.get_at(pos_perm_pixel)

    # Returns the permuted pixel position.
    def permute_pixel_position(pos_pixel, image_resolution, rgn_perms, rgn_schemes):
        # If there are no more levels of permutations to apply return the original position
        if len(rgn_perms) <= 0 or len(rgn_perms) <= 0:
            return pos_pixel
        # At this level we will apply the first permutation with the first region scheme
        perm = rgn_perms[0]
        rgn_scheme = rgn_schemes[0]
        # Calculate size of the regions
        rgn_size = [ image_resolution[0] // rgn_scheme[0], image_resolution[1] // rgn_scheme[1] ]
        # If regions turn out to be smaller than 1 pixel
        # this means we can't subdivide anymore.
        # In this case return the original position
        if rgn_size[0] <= 0 or rgn_size[1] <= 0:
            print("WARNING: Regions for the ImagePermuter go too deep")
            return pos_pixel
        # Find the region where the original position is - calculate the region's 2D index
        rgn_og_idx_twodim = [ pos_pixel[0] // rgn_size[0], pos_pixel[1] // rgn_size[1] ]
        # If the pixel position is in the remainder part of the regions, we cannot permute it normally.
        # A remainder region cannot be swapped with a normal region because they are of different sizes.
        # TODO: Think of some solution. For now just return the original index
        if rgn_og_idx_twodim[0] >= rgn_scheme[0] or rgn_og_idx_twodim[1] >= rgn_scheme[1]:
            return pos_pixel
        # Convert the 2D index of the region to a 1D index so that the permutation can be applied on it
        rgn_og_idx_onedim = rgn_og_idx_twodim[1] * rgn_scheme[0] + rgn_og_idx_twodim[0]
        # Apply the permutation on the 1D index of the region to get the 1D index of the permuted region
        rgn_perm_idx_onedim = perm[rgn_og_idx_onedim]
        # Convert the 1D index of the permuted region to a 2D index
        rgn_perm_idx_twodim = [ rgn_perm_idx_onedim % rgn_scheme[0], rgn_perm_idx_onedim // rgn_scheme[0] ]
        # Calculate the position of the pixel relative to the region
        pos_pixel_in_region = [
            pos_pixel[0] % rgn_size[0],
            pos_pixel[1] % rgn_size[1]
        ]
        # Recursively permute the pixel inside the region
        # by subdividing the region into smaller regions and permuting them with the next permutation
        pos_perm_pixel_in_region = ImagePermuter.permute_pixel_position(
            pos_pixel_in_region,
            rgn_size,
            rgn_perms[1:],
            rgn_schemes[1:]
        )
        # Calculate the final pixel position by adding the permuted outer region
        # to the position inside the permuted inner region
        pos_perm_pixel = [
            rgn_perm_idx_twodim[0] * rgn_size[0] + pos_perm_pixel_in_region[0],
            rgn_perm_idx_twodim[1] * rgn_size[1] + pos_perm_pixel_in_region[1]
        ]
        return pos_perm_pixel