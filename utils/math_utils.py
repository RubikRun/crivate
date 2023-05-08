# Class for helpful mathematical functions.
class MathUtils:
    # Calculates the factorial of a number
    def factorial(n):
        # For n=0 and n=1 the factorial is 1
        if n < 1:
            return 1
        # Otherwise start from 2,
        result = 2
        # traverse all numbers 3,4,...,n
        for i in range(3, n+1):
            # and multiply the result by each one
            result *= i
        # At the end we have 2*3*4*...*n
        return result

    # Returns the permutation with the given lexicographical index,
    # as a list of numbers 0,1,...,n-1 in the order of the premutation.
    # Example: For perm_size=3 the permutations in lexicographical order are:
    #     012, 021, 102, 120, 201, 210
    # The 0-th permutation with size 3 is [0, 1, 2] and the 5-th one is [2, 1, 0].
    # So for example get_perm_from_index(3, 2) should return [1, 0, 2].
    def get_perm_from_index(perm_size, index):
        # If the size is 0 or 1 there is only one permutation. Directly return it
        if perm_size == 0:
            return []
        if perm_size == 1:
            return [0]
        # Permutation will be filled to this list
        perm = []
        # Create a list for marking which elements have been used so far.
        # In the beginning no elements are used, so fill it with False.
        used = [False] * perm_size
        # Let N be the permutation size. We group the permutations based on their first digit.
        # Doing that results in N groups, each with N!/N=(N-1)! permutations in it.
        # What's more, in the lexicographical order, by definition,
        # the first (N-1)! permutations begin with 0 and form group 0,
        # the next (N-1)! permutations begin with 1 and form group 1, and so on.
        group_size = MathUtils.factorial(perm_size - 1)
        # As we go we will fill elements to the perm list.
        # At any point the already filled elements will limit the space of possible permutations.
        # The "limited" index will keep track of the index of the target permutation inside this limited space.
        # So the limited index will be the index of the permutation
        # amongst the permutations starting with the elements currently in perm.
        limited_index = index
        # We will group the remaining permutations first by the 1st digit, then by 2nd, and so on.
        # This will reduce the group size at each step,
        while True:
            # We find the index of the group by dividing the limited index by the group size
            group_index = limited_index // group_size
            # The next element in the permutaion will be the group_size-th unused element
            elem = 0
            while group_index > 0 or used[elem]:
                if not used[elem]:
                    group_index -= 1
                elem += 1
            # Add the element to the permutation
            perm.append(elem)
            # and mark it as used
            used[elem] = True
            # The newly added element limits the space of permutations and we need to update the limited index.
            limited_index %= group_size
            # Reduce the group size to match the limited space.
            # Now we are grouping by the next digit, since the current digit is already known.
            if group_size > 1:
                group_size /= (perm_size - len(perm))
            # When we reach a group size of 1 we cannot limit no more, we did just put the second-to-last element.
            # So we break out of the cycle and there's one more element left.
            else:
                break
        # At this point all elements but one are added.
        # We just add the last one at the end of the permutation.
        for elem in range(perm_size):
            if not used[elem]:
                perm.append(elem)
                break
        return perm