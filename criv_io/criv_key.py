from utils.math_utils import MathUtils
from random import randint

# Class for a criv key.
# A criv key is a key for encrypting and decrypting a .criv image file
class CrivKey:
    CRIV_KEY_SEPARATOR = ','
    RGN_SCHEME_DEFAULT = [[24, 24], [16, 16], [8, 8], [4, 4], [4, 4], [4, 4], [4, 4]]

    # Creates a new criv key with the given data
    #   - levels_count is the level of recursion for subdividing the image into regions
    #   - rgn_schemes are the region schemes for each level of subdivision.
    #     Each region scheme is just two numbers - the number of columns and rows.
    #   - rgn_perms are the region permutations for each level of subdivision.
    #     Each region permutation is a permutation of the regions in the level.
    #   - color_perm is a permutation of the 256 possible values for a single color component.
    def __init__(self, levels_count, rgn_schemes, rgn_perms, color_perm):
        if levels_count < 1:
            print("ERROR Criv key should consist of at least 1 level")
        self.levels_count = levels_count

        if levels_count != len(rgn_schemes) or len(rgn_schemes) != len(rgn_perms):
            print("ERROR Criv key should have equal number of levels, region schemes and region permutations")

        for i in range(levels_count):
            if len(rgn_schemes[i]) != 2:
                print("ERROR Criv key region scheme should consist of exactly 2 numbers - number of rows and columns")
            if rgn_schemes[i][0] < 1 or rgn_schemes[i][1] < 1:
                print("ERROR Criv key region scheme's numbers of rows and columns should be at least 1")
            if len(rgn_perms[i]) == 0:
                print("ERROR Criv key has an empty region permutation")
            if rgn_schemes[i][0] * rgn_schemes[i][1] != len(rgn_perms[i]):
                print("ERROR Criv key region scheme's numbers of rows and columns should multiply to the length of the region permutation")

        self.rgn_schemes = rgn_schemes
        self.rgn_perms = rgn_perms
        self.color_perm = color_perm

    # Writes the criv key to a .crivkey file
    def write_to_file(self, filepath):
        with open(filepath, 'w') as file:
            # Write the number of levels
            CrivKey.__write_num(file, self.levels_count)
            # Write the region schemes one by one, first cols then rows
            for level in range(self.levels_count):
                CrivKey.__write_num(file, self.rgn_schemes[level][0])
                CrivKey.__write_num(file, self.rgn_schemes[level][1])
            # Write the index of each region permutation one by one
            for level in range(self.levels_count):
                CrivKey.__write_num(file, MathUtils.get_index_from_perm(self.rgn_perms[level]))
            # Write the index of the color permutation
            CrivKey.__write_num(file, MathUtils.get_index_from_perm(self.color_perm))
    
    # Reads the criv key from a .crivkey file
    def read_from_file(filepath):
        with open(filepath, 'r') as file:
            crivkey_data = file.read().split(CrivKey.CRIV_KEY_SEPARATOR)
            # Get the number of levels from the crivkey data
            levels_count = int(crivkey_data[0])
            # Extract the region schemes from the crivkey data
            rgn_schemes = []
            for level in range(levels_count):
                rgn_scheme = [-1, -1]
                rgn_scheme[0] = int(crivkey_data[1 + level * 2 + 0])
                rgn_scheme[1] = int(crivkey_data[1 + level * 2 + 1])
                rgn_schemes.append(rgn_scheme)
            # Extract the index of each region permutation and transform the index to a permutation
            rgn_perms = []
            for level in range(levels_count):
                perm_size = rgn_schemes[level][0] * rgn_schemes[level][1]
                perm_idx = int(crivkey_data[1 + levels_count * 2 + level])
                perm = MathUtils.get_perm_from_index(perm_size, perm_idx)
                rgn_perms.append(perm)
            # Extract the index of the color permutation and transform it to a permutation
            color_perm_idx = int(crivkey_data[1 + levels_count * 3])
            color_perm = MathUtils.get_perm_from_index(256, color_perm_idx)

            # Construct and return a CrivKey object from the read data
            criv_key = CrivKey(levels_count, rgn_schemes, rgn_perms, color_perm)
            return criv_key
    
    # Generates an identity key such that encrypting/decrypting with it does nothing to an image
    def generate_identity_key():
        levels_count = 1
        rgn_schemes = [[1, 1]]
        rgn_perms = [MathUtils.get_perm_from_index(1, 0)]
        color_perm = MathUtils.get_perm_from_index(256, 0)
        # Construct and return a CrivKey object from the generated data
        criv_key = CrivKey(levels_count, rgn_schemes, rgn_perms, color_perm)
        return criv_key

    def generate_random_key(image_resolution):
        # Count the number of region schemes to use from the default region schemes.
        # Use maximum levels while keeping the number of pixels
        # in the smallest level region to be at least 1.
        # Calculate this separately for rows and columns
        fixed_pixels = image_resolution
        default_rgn_schemes_count = [0, 0]
        while fixed_pixels[0] >= 1:
            fixed_pixels[0] /= CrivKey.RGN_SCHEME_DEFAULT[default_rgn_schemes_count[0]][0]
            if fixed_pixels[0] >= 1:
                default_rgn_schemes_count[0] += 1
        while fixed_pixels[1] >= 1:
            fixed_pixels[1] /= CrivKey.RGN_SCHEME_DEFAULT[default_rgn_schemes_count[1]][1]
            if fixed_pixels[1] >= 1:
                default_rgn_schemes_count[1] += 1
        # The number of subdivision levels will be the max of the default region schemes used for cols and rows.
        # There might be a case with an image that has width quite bigger than its height or vice versa,
        # for example an image with resolution 1000 x 10.
        # In this case it might make sense to subdivide width 3 times but subdivide height only once.
        # So the total number of region schemes will be max(3,1) = 3.
        # The last 2 region schemes will have number of rows = 1,
        # which is not really a subdivision but it's a valid region scheme.
        print("default_rgn_schemes_count = ", default_rgn_schemes_count)
        levels_count = max(default_rgn_schemes_count[0], default_rgn_schemes_count[1])
        # Create the region schemes from the default region schemes,
        # filling the rest of one of the components with 1s if needed
        rgn_schemes = []
        for level in range(levels_count):
            rgn_scheme = [1, 1]
            if level < default_rgn_schemes_count[0]:
                rgn_scheme[0] = CrivKey.RGN_SCHEME_DEFAULT[level][0]
            if level < default_rgn_schemes_count[1]:
                rgn_scheme[1] = CrivKey.RGN_SCHEME_DEFAULT[level][1]
            rgn_schemes.append(rgn_scheme)

        # Generate random region permutation for each level
        rgn_perms = []
        for level in range(levels_count):
            # Calculate the permutation size as the number of regions in the scheme of the current level
            perm_size = rgn_schemes[level][0] * rgn_schemes[level][1]
            # Generate a random permutation index without 0 because 0 is the identity permutation
            # TODO: maybe also don't allow permutations that leave >50% of the elements in place
            perm_idx = randint(1, MathUtils.factorial(perm_size) - 1)
            # Transform index to permutation
            perm = MathUtils.get_perm_from_index(perm_size, perm_idx)
            # Add to the list of region permutations
            rgn_perms.append(perm)

        # Generate random color permutation
        # TODO: maybe also don't allow permutations that leave >50% of the elements in place
        color_perm_idx = randint(1, MathUtils.factorial(256) - 1)
        # Transform index to permutation
        color_perm = MathUtils.get_perm_from_index(256, color_perm_idx)

        # Construct and return a CrivKey object from the generated data
        criv_key = CrivKey(levels_count, rgn_schemes, rgn_perms, color_perm)
        return criv_key

    # Returns the inverse key of this criv key.
    # If this key was used for encryption, the inverse can be used for decryption, and vice versa
    def get_inverse_key(self):
        inv_rgn_perms = []
        for level in range(self.levels_count):
            inv_rgn_perm = MathUtils.get_inverse_perm(self.rgn_perms[level])
            inv_rgn_perms.append(inv_rgn_perm)
        inv_color_perm = MathUtils.get_inverse_perm(self.color_perm)

        inv_criv_key = CrivKey(self.levels_count, self.rgn_schemes, inv_rgn_perms, inv_color_perm)
        return inv_criv_key

    # Writes a single number to an opened .crivkey file
    def __write_num(file, num):
        file.write(str(num) + CrivKey.CRIV_KEY_SEPARATOR)