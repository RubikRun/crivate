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
    
    # Returns the inverse permutation of a given permutation.
    # If the given permutation is A and the returned inverse is B,
    # then for all valid indices x we have:
    #   B[A[x]] = A[B[x]] = x
    def get_inverse_perm(perm):
        perm_size = len(perm)
        # This will be the inverse permutation.
        # It will be of the same size as the original permutation.
        # Initialize with an invalid value for each element.
        inverse = [-1] * perm_size
        # Traverse the elements
        for elem in range(perm_size):
            # Where the original permutation takes the element
            elem_after_perm = perm[elem]
            # The inverse permutation should take this element to the original element
            inverse[elem_after_perm] = elem
        return inverse

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
                group_size //= (perm_size - len(perm))
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

    # Returns the lexicographical index of the given permutation.
    # The given permutation is expected to be a list of the numbers 0,1,..,N in some order.
    # Example: For size 3 the permutations in lexicographical order are:
    #     012, 021, 102, 120, 201, 210
    # The 0-th permutation with size 3 is [0, 1, 2] and the 5-th one is [2, 1, 0].
    # So for example get_index_from_perm([1, 0, 2]) should return 2.
    def get_index_from_perm(perm):
        # The algorithm will be the following:
        # For each element in the permutation, starting from the 0-th one to the last one,
        # we group permutations based on their element at that index.
        # We consider the elements that we already passed to be fixed,
        # and look only at permutations starting with those exact elements.
        # The value of each element can tell us in which group the permutation is,
        # but we don't use the value directly, instead we need to count how many
        # of the smaller elements are not already used, because they will be inside the non-fixed part.
        # After finding the group index we multiply it by the size of the group
        # to get the total number of permutations that are inside previous groups,
        # and hence in previous permutations, so they need to be counted by the lexicographical index.
        # Counting permutations of all such groups until the last element, when there is only one group,
        # gives us the final index.
        perm_size = len(perm)
        # This will be the resulting lexicographical index.
        lex_index = 0
        # Current element's index
        elem_index = 0
        # Current group's size when grouping with the current element
        group_size = MathUtils.factorial(perm_size - 1)
        # Marking used elements. In the beginning no element is yet used, so fill with False.
        used = [False] * perm_size
        # Traverse elements until the second-to-last one.
        # The last one will give us only a single group and will contribute 0 to the index.
        while elem_index + 1 < perm_size:
            elem = perm[elem_index]
            # Find the group in which the current element is
            # by counting how many smaller elements are unused
            group_index = 0
            for smaller_elem in range(elem):
                if not used[smaller_elem]:
                    group_index += 1
            # Multiplying the group index by the size of the group
            # gives us the number of permutations inside previous groups.
            # We need to count those with the lex index.
            lex_index += group_size * group_index
            # Decrease the group size for the next element in the traversal
            group_size //= (perm_size - 1 - elem_index)
            # Mark the element as used
            used[elem] = True
            # Next element
            elem_index += 1
        # At this point permutations from all previous groups are counted,
        # and these are all previous permutations, which gives us exactly the lexicographical index.
        return lex_index