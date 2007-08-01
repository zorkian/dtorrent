
                             Enhanced CTorrent

      [ [1]Info | [2]Notes | [3]Changes | [4]Download | [5]Resources |
                               [6]Contact ] 

Information

   [7]CTorrent is a [8]BitTorrent client implemented in C++ to be
   lightweight and quick. It has fallen a little behind in updates and
   bug fixes though.

   The files here contain the good work of those who wrote the original
   CTorrent base code and a number of patches that provide fixes and
   enhancements, as well as additional fixes and enhancements that I am
   contributing. I am not the original author, current maintainer, or any
   other official representative of CTorrent. The files on this page are
   not the original or official CTorrent distribution. I encourage you to
   visit the [9]CTorrent project page on SourceForge for further
   information.

Notes

   Beginning with dnh1.1 the default peer ID has been changed for
   convenience, as some other clients and trackers assume that Ctorrent
   is "buggy" and won't cooperate with it. [Guess what, there are plenty
   of others with bugs too.] The -P option is still available if you wish
   to use a different peer ID, but it is no longer necessary to do so in
   order to avoid this "ban".

Changes for "dnh1.1" Release

   These are just corrections to the previous release that I felt were
   necessary. Much more improvement is coming in the next release.

   Bug/code fixes
     * Peer count would increase on each tracker update if there were no
       seeders.
     * RequestQueue::CopyShuffle() changed to use a pointer argument.
     * Fixed some incorrectness in PendingQueue::Delete() and
       PendingQueue::DeleteSlice() which could cause a memory leak.
     * Fixed random-chance inversion bug in PeerList::UnChokeCheck()
       affecting choice for optimistic unchoking.

   Improvements
     * Move StopDLTimer() call from RequestPiece() to RequestCheck(),
       which could occasionally affect peer download rate measurement.
     * Most clients do not like a slice size of 128K even though it is
       the max allowed by the BT specification. Changed max slice size to
       64K. Note that the maximum piece length is 2MB (2097152); if you
       need to download a torrent with a larger piece size you can change
       the value of cfg_req_queue_length in btconfig.h from 64 to 128.
     * Contact tracker immediately upon becoming (or starting as) a
       seeder.
     * Changed SendModule() to send only one slice at a time. This will
       help with fairly distributing upload bandwidth among the unchoked
       peers.
     * Changed default peer ID prefix to '-CD0101-', indicating
       CTorrent-dnh1.1 release.

Changes for "dnh1" Release

   This is the first release. "dnh" identifies this patchset, and "1"
   indicates release version 1 of the patchset.

   Patches
     * Incorporates the following patches. The number is the Request ID
       from the [10]SourceForge patches page, which you can reference for
       the details of each patch. The name in brackets is the name of the
       patch file or a name I chose to refer to the patch. Some of these
       names are used below (in brackets) to describe a fix or change to
       a particular patch.
       1042808 [getcwd] (incorporated in get1file patch)
       1084776 [passkey] (incorporated in udlimit patch)
       1109266 [align]
       1109287 [tracker/tracker2]
       1114197 [fmt] (incorporated in get1file patch)
       1114364 [resetdl]
       1116448 [get1file]
       1118597 [crash]
       1119467 [stall]
       1119492 [rate]
       1119497 [flush]
       1119519 [opt]
       1119689 [status]
       1124342 [udlimit]

   Download performance
     * If a peer socket is ready for reading and writing, perform both.
       Previously the cases were exclusive, with preference given to
       reading.
     * Download requests are now made to peers when they are ready for
       writing (in addition to the existing event-driven cases). This
       fixes peer stalls when a request couldn't be sent in an
       event-triggered case due to bandwidth limiting or other
       circumstances.
     * Additional tests added so that the above request checking doesn't
       create hard loops.

   Bandwidth measurement/management
     * [rate] Bandwidth reporting is now not capped. Also, only the time
       used for the samples taken is used in the calculation rather than
       the maximum interval (this affects rate calculation for individual
       peers).
     * Additional upload and download bandwidth limit checks added so
       that bw management is more accurate.
     * Corrected condition inversion bug in Rate::StopTimer(), which
       affects peer rate calculations.

   Peer count
     * Request our max number of peers from the tracker each time rather
       than just taking the tracker's default, so we can try to fill up.
     * The tracker will be contacted early if all peers disconnect so
       that we can actively try to establish some more peer connections.
       To avoid hammering the tracker, we must have at least one peer for
       15 seconds in order for this to be invoked.
     * Some clients use nonzero bytes in the "reserved" part of the
       handshake. Added code to ignore the reserved bytes if the rest of
       the handshake is as expected. This includes Azureus 2300 thru 2304
       (latest) which gives 0x80 as the first reserved byte and BitComet
       which gives 0x6578 ("ex") as the first two bytes.
     * Update peer's timestamp on any message, not just keepalives. Any
       receipt of data from a peer now resets its timeout, preventing
       early disconnect.

   Parallel requests
     * Initial-piece and endgame cases have been improved so that pieces
       will be requested from multiple peers. Cancels are sent as slices
       (subpieces) are received. This endgame strategy is described in
       the BitTorrent online spec. The startup strategy is also described
       in posts and facilitates obtaining a single piece more rapidly in
       order to obtain some trade value. In initial mode, the piece of
       which we need the least parts is targeted. In endgame mode, the
       piece of which we need the most parts is targeted. Slices that are
       cancelled are also removed from the "pending" queue, as are pieces
       that are completed.
     * When duplicating a piece request, the slice request order is also
       shuffled in order to minimize duplication of effort.

   Tracker info
     * [status] Seems to be missing tracker.cpp diff. Added code to get
       tracker's total peers, but not all trackers report these fields in
       the normal response. Also added code to count successful updates
       from the tracker.
     * Added tracker connection state to status line (when
       connecting/connected).

   Tracker contact
     * When interrupting (ctrl-C), connect to the tracker immediately
       rather than waiting 15 seconds.
     * Contact tracker "soon" after transitioning to seeder state.

   Peer interaction
     * Manage our interested state for each peer dynamically as content
       changes.
     * Unchoking
          + Use the peer's interested state (confirming via the bitfield
            that it needs our data) to consider unchoking. Using only the
            bitfield could cause us to waste an upload slot on a peer
            that doesn't have all content but isn't interested (like a
            single-file downloader).
          + Choke peers that become uninterested or don't need our data.
          + Try unchoking new peers only as slots open or the optimistic
            unchoke rotates. Unchoking too many peers can temporarily
            reduce per-peer upload rates, which would make uploading to
            us unappealing for good peers.
          + In a tie for download speed, prefer to unchoke the peer to
            whom we've uploaded the least data relative to what we've
            downloaded from him.
     * Optimistic unchoking
          + Fixed condition inversion bug causing opt unchoking to occur
            too often.
          + A peer who has no pieces is now preferred 75% vs. a peer who
            already has at least one piece, in order to help the new peer
            become productive. This is documented in the online spec.
          + Set peer's last-unchoke-time when choking the peer to get
            better rotation of the optimistic unchoke. The value is now
            the last time that a peer was in the unchoked state rather
            than the time of the last unchoke event.

   Miscellaneous items
     * [tracker] Fixed normal program end (stop) process, which was
       crashing.
     * [get1file] Restore compact tracker response support.
     * [get1file] Made display of the status line info dependent on
       whether the option is in use.
     * [get1file] Apply the filter when checking for what we need from a
       peer instead of when recording what the peer has; this prevents
       stalls when we move on to the next file. Update interested state
       for each peer when we begin a new file. Also move the file-done
       check into the piece completion code and check whether the next
       file(s) has also been completed.
     * Reduced the slice size from 32K to 16K (same as BT & Azureus).
       This provides more precise DL rate measurement, and helps insure
       that we receive a productive amount of data (i.e. a complete
       slice) even if we are unchoked by a peer for only one cycle. See
       [11]http://groups.yahoo.com/group/BitTorrent/message/1260 for more
       discussion/analysis on this.
     * Added -v (verbose) option for additional debugging output.

Download

   Release dnh1.1

   [12]dnh1 to dnh1.1 patch file
   A patch file of changes to release dnh1 to bring it up to dnh1.1.
                    ___________________________________

   Release dnh1

   [13]FreeBSD patch file
   A patch file of changes to the CTorrent 1.3.4 base, including the
   patches from the FreeBSD ports tree.
   Note: Thanks to Florent Thoumie, as of 29 Jul 2005 this patchset is
   included in the FreeBSD port. If you update your ports tree (or at
   least net/ctorrent) and install from there, you will have these
   updates without downloading the file and patching manually.

   [14]Patch file
   A patch file of changes to the CTorrent 1.3.4 base.

   [15]FreeBSD patched source
   This includes the patches from the FreeBSD ports tree.

   [16]Linux/Windows/Other patched source
   Please [17]let me know if you encounter any portability issues, as I
   don't have a test environment set up for these platforms.

Resources

   [18]CTorrent Home Page 
   Outdated, but you may find some useful info (particularly the FAQ).

   [19]CTorrent SourceForge Project 
   Hosts the CTorrent codebase, bug reports, patches, and forum.

   [20]Custom CTorrent 
   A page by the author of the "get1file" patch and other fixes. It
   contains a custom version and a GUI for CTorrent.

   [21]BitTorrent 
   The official BitTorrent home page.

   [22]BitTorrent wiki 
   Various documentation.

   [23]BitTorrent protocol specification (official version)

   [24]BitTorrent protocol specification (wiki version)

References

   1. http://www.rahul.net/dholmes/ctorrent/index.html#info
   2. http://www.rahul.net/dholmes/ctorrent/index.html#notes
   3. http://www.rahul.net/dholmes/ctorrent/index.html#changes
   4. http://www.rahul.net/dholmes/ctorrent/index.html#download
   5. http://www.rahul.net/dholmes/ctorrent/index.html#resources
   6. mailto:dholmes@ct.boxmail.com
   7. http://ctorrent.sourceforge.net/
   8. http://www.bittorrent.com/
   9. http://sourceforge.net/projects/ctorrent/
  10. http://sourceforge.net/tracker/?atid=598034&group_id=91688&func=browse
  11. http://groups.yahoo.com/group/BitTorrent/message/1260
  12. http://www.rahul.net/dholmes/ctorrent/patchset-ctorrent-dnh1-dnh1.1.diff
  13. http://www.rahul.net/dholmes/ctorrent/patchset-ctorrent-1.3.4-dnh1-fbsd.diff
  14. http://www.rahul.net/dholmes/ctorrent/patchset-ctorrent-1.3.4-dnh1.diff
  15. http://www.rahul.net/dholmes/ctorrent/ctorrent-1.3.4-dnh1-fbsd.tar.gz
  16. http://www.rahul.net/dholmes/ctorrent/ctorrent-1.3.4-dnh1.tar.gz
  17. mailto:dholmes@ct.boxmail.com
  18. http://ctorrent.sourceforge.net/
  19. http://sourceforge.net/projects/ctorrent/
  20. http://customctorrent.ifreepages.com/
  21. http://bittorrent.com/
  22. http://wiki.theory.org/CategoryBitTorrent
  23. http://www.bittorrent.com/protocol.html
  24. http://wiki.theory.org/BitTorrentSpecification
