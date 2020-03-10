FFmpeg Unmerged Patches
========================

** How to update patches

    Please put patchset in this folder.
    
** How to submit pull request

    Please submit pull request for this folder.

** How to apply patches

    $ git am patchset/*.patch
    or
    $ for i in patchset/*.patch; do patch -p1 < $i; done

    Note: CI server use 'patch -t' to apply patches


Please contact zachary.zhou@intel.com if you have any questions.
