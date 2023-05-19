# Class used to apply permutations to pixels of an image
class ImagePermuter:
    # Returns the color at a given pixel position in an image with an applied permutation.
    # The pixels of the image are separated into regions.
    # The number of those regions horizontally and vertically should be provided in the rgn_count parameter.
    # Those regions are then permuted/shuffled with the given permutation.
    # The function returns the color inside that shuffled image.
    def get_color_at(pos_pixel, image, perm, rgn_count):
        # Get the resolution of the image
        image_resolution = [image.get_width(), image.get_height()]
        # Permute the pixel position to get the position of the pixel after applying the permutation
        pos_perm_pixel = ImagePermuter.permute_pixel_position(pos_pixel, image_resolution, perm, rgn_count)
        # Return the image color of the permuted pixel
        return image.get_at(pos_perm_pixel)

    # Returns the permuted pixel position.
    def permute_pixel_position(pos_pixel, image_resolution, perm, rgn_count):
        # Check if the horizontal and vertical region counts make sense
        if len(rgn_count) < 2 or rgn_count[0] * rgn_count[1] != len(perm):
            print("ERROR: The numbers of regions horizontally and vertically should" +
                + " multiply to the length of the permutation.")
            return [0, 0, 0]
        # Calculate size of the regions
        rgn_size = [ image_resolution[0] // rgn_count[0], image_resolution[1] // rgn_count[1] ]
        # Find the region where the original position is - calculate the region's 2D index
        rgn_og_idx_twodim = [ pos_pixel[0] // rgn_size[0], pos_pixel[1] // rgn_size[1] ]
        # If the pixel position is in the remainder part of the regions, we cannot permute it normally.
        # A remainder region cannot be swapped with a normal region because they are of different sizes.
        # TODO: Think of some solution. For now just return the original index
        if rgn_og_idx_twodim[0] >= rgn_count[0] or rgn_og_idx_twodim[1] >= rgn_count[1]:
            return pos_pixel
        # Convert the 2D index of the region to a 1D index so that the permutation can be applied on it
        rgn_og_idx_onedim = rgn_og_idx_twodim[1] * rgn_count[0] + rgn_og_idx_twodim[0]
        # Apply the permutation on the 1D index of the region to get the 1D index of the permuted region
        rgn_perm_idx_onedim = perm[rgn_og_idx_onedim]
        # Convert the 1D index of the permuted region to a 2D index
        rgn_perm_idx_twodim = [ rgn_perm_idx_onedim % rgn_count[0], rgn_perm_idx_onedim // rgn_count[0] ]
        # Calculate the position of the permuted pixel
        pos_perm_pixel = [
            rgn_perm_idx_twodim[0] * rgn_size[0] + pos_pixel[0] % rgn_size[0],
            rgn_perm_idx_twodim[1] * rgn_size[1] + pos_pixel[1] % rgn_size[1]
        ]
        return pos_perm_pixel