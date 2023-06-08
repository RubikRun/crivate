class CrivKey:
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