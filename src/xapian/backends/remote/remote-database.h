/** @file remote-database.h
 *  @brief RemoteDatabase is the baseclass for remote database implementations.
 */
/* Copyright (C) 2006,2007,2009,2010,2011,2014,2015,2017 Olly Betts
 * Copyright (C) 2007,2009,2010 Lemur Consulting Ltd
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */

#ifndef XAPIAN_INCLUDED_REMOTE_DATABASE_H
#define XAPIAN_INCLUDED_REMOTE_DATABASE_H

#include "xapian/backends/backends.h"
#include "xapian/backends/databaseinternal.h"
#include "xapian/api/enquireinternal.h"
#include "xapian/api/queryinternal.h"
#include "xapian/net/remoteconnection.h"
#include "xapian/backends/valuestats.h"
#include "xapian/weight.h"

namespace Xapian {
    class RSet;
}

class NetworkPostList;

/** RemoteDatabase is the baseclass for remote database implementations.
 *
 *  A subclass of this class is required which opens a TCP connection or
 *  pipe to the remote database server.  This subclass works in combination
 *  with the RemoteSubMatch class during the match process.
 */
class RemoteDatabase : public Xapian::Database::Internal {
    /// Don't allow assignment.
    void operator=(const RemoteDatabase &);

    /// Don't allow copying.
    RemoteDatabase(const RemoteDatabase &);

    // Directory to store databases in.
    const std::string db_dir;

    // The database is writable.
    const bool writable;

    // Bitwise-or of Xapian::DB_* flags.
    const int flags;

    /// The object which does the I/O.
    mutable RemoteConnection link;

    /// The remote document count, given at open.
    mutable Xapian::doccount doccount;

    /// The remote last docid, given at open.
    mutable Xapian::docid lastdocid;

    /// A lower bound on the smallest document length in this database.
    mutable Xapian::termcount doclen_lbound;

    /// An upper bound on the greatest document length in this database.
    mutable Xapian::termcount doclen_ubound;

    /// The total length of all documents in this database.
    mutable Xapian::totallength total_length;

    /// Has positional information?
    mutable bool has_positional_info;

    /// The UUID of the remote database.
    mutable std::string uuid;

    /// The context to return with any error messages
    std::string context;

    mutable bool cached_stats_valid;

    /** The most recently used value statistics. */
    mutable ValueStats mru_valstats;

    /** The value slot for the most recently used value statistics.
     *
     *  Set to BAD_VALUENO if no value statistics have yet been looked up.
     */
    mutable Xapian::valueno mru_slot;

    bool update_stats(message_type msg_code = MSG_UPDATE,
		      const std::string & body = std::string()) const;

  protected:
    /** Constructor.  The constructor is protected so that raw instances
     *  can't be created - a derived class must be instantiated which
     *  has code in the constructor to open the socket.
     *
     *  @param fd	The file descriptor for the connection to the server.
     *  @param timeout_ The timeout used with the network operations.
     *			Generally a Xapian::NetworkTimeoutError exception will
     *			be thrown if the remote end doesn't respond for this
     *			length of time (in seconds).  A timeout of 0 means that
     *			operations will never timeout.
     *  @param context_ The context to return with any error messages.
     *  @param writable	Is this a WritableDatabase?
     *  @param flags	Xapian::DB_RETRY_LOCK or Bitwise-or of Xapian::DB_* constants.
     *  @param dir      Database directory index to open.
     */
    RemoteDatabase(int fd, double timeout_, const std::string& context_,
		   bool writable, int flags, const std::string& dir);

    /// Receive a message from the server.
    reply_type get_message(std::string& message,
			   reply_type required_type,
			   reply_type required_type2) const;

    void get_message(std::string& message,
		     reply_type required_type) const {
	(void)get_message(message, required_type, required_type);
    }

    bool get_message_or_done(std::string& message,
			     reply_type required_type) const {
	return get_message(message, required_type, REPLY_DONE) != REPLY_DONE;
    }

    /// Send a message to the server.
    void send_message(message_type type, const std::string& data) const;

    /// Close the socket
    void do_close();

    bool get_posting(Xapian::docid& did, double& w, std::string& value);

    /// The timeout value used in network communications, in seconds.
    double timeout;

  public:
    /** Get the length of the position list.
     *
     *  Extra method for RemoteDatabase.
     */
    Xapian::termcount positionlist_count(Xapian::docid did,
					 const std::string& term) const;

    /// Send a keep-alive message.
    void keep_alive();

    typedef Xapian::Internal::opt_intrusive_ptr<Xapian::MatchSpy> opt_ptr_spy;

    typedef Xapian::Internal::opt_intrusive_ptr<Xapian::KeyMaker> opt_ptr_sorter;

    /** Set the query
     *
     * @param query			The query.
     * @param qlen			The query length.
     * @param collapse_key		The value number to collapse matches on.
     * @param collapse_max		Max number of items with the same key
     *					to leave after collapsing (0 for don't
     *					collapse).
     * @param order			Sort order for docids.
     * @param sort_key			The value number to sort on.
     * @param sort_by			Which order to apply sorts in.
     * @param sort_value_forward	Sort order for values.
     * @param time_limit_		Seconds to reduce check_at_least after
     *					(or <= 0 for no limit).
     * @param percent_threshold		Lower bound on percentage score.
     * @param weight_threshold		Lower bound on weight.
     * @param wtscheme			Weighting scheme.
     * @param omrset			The rset.
     * @param matchspies                The matchspies to use.
     */
    void set_query(const Xapian::Query& query,
		   Xapian::termcount qlen,
		   Xapian::valueno collapse_key,
		   Xapian::doccount collapse_max,
		   Xapian::Enquire::docid_order order,
		   Xapian::valueno sort_key,
		   Xapian::Enquire::Internal::sort_setting sort_by,
		   bool sort_value_forward,
		   double time_limit,
		   int percent_threshold, double weight_threshold,
		   const Xapian::Weight& wtscheme,
		   const Xapian::RSet &omrset,
		   const std::vector<opt_ptr_spy>& matchspies,
		   const Xapian::KeyMaker* sorter) const;

    /** Get the underlying fd this remote connection reads from.
     *
     *  This allows the matcher to efficiently wait for remote databases to be
     *  ready in parallel using poll() or select().
     */
    int get_read_fd() const {
	return link.get_read_fd();
    }

    /// Get the stats from the remote server.
    void get_remote_stats(Xapian::Weight::Internal& out) const;

    /// Send the global stats to the remote server.
    void send_global_stats(Xapian::doccount first,
			   Xapian::doccount maxitems,
			   Xapian::doccount check_at_least,
			   const Xapian::Weight::Internal &stats) const;

    /// Get the MSet from the remote server.
    Xapian::MSet get_mset(const std::vector<opt_ptr_spy>& matchspies) const;

    /// Get remote metadata key list.
    TermList * open_metadata_keylist(const std::string & prefix) const;

    /// Get remote termlist.
    TermList * open_term_list(Xapian::docid did) const;

    TermList * open_term_list_direct(Xapian::docid did) const;

    /// Iterate all terms.
    TermList * open_allterms(const std::string& prefix) const;

    bool has_positions() const;

    bool reopen();

    void close();

    PostList* open_post_list(const std::string& term) const;

    LeafPostList* open_leaf_post_list(const std::string& term, bool) const;

    Xapian::doccount read_post_list(const std::string& term, NetworkPostList & pl) const;

    PositionList * open_position_list(Xapian::docid did,
				      const std::string& tname) const;

    /// Get a remote document.
    Xapian::Document::Internal * open_document(Xapian::docid did, bool lazy) const;

    /// Get the document count.
    Xapian::doccount get_doccount() const;

    /// Get the last used docid.
    Xapian::docid get_lastdocid() const;

    Xapian::totallength get_total_length() const;

    Xapian::termcount get_doclength(Xapian::docid did) const;
    Xapian::termcount get_unique_terms(Xapian::docid did) const;

    /// Check if term exists.
    bool term_exists(const std::string& tname) const;

    void get_freqs(const std::string& term,
		   Xapian::doccount * termfreq_ptr,
		   Xapian::termcount * collfreq_ptr) const;

    /// Read the value statistics for a value from a remote database.
    void read_value_stats(Xapian::valueno slot) const;
    Xapian::doccount get_value_freq(Xapian::valueno slot) const;
    std::string get_value_lower_bound(Xapian::valueno slot) const;
    std::string get_value_upper_bound(Xapian::valueno slot) const;

    Xapian::termcount get_doclength_lower_bound() const;
    Xapian::termcount get_doclength_upper_bound() const;
    Xapian::termcount get_wdf_upper_bound(const std::string& term) const;

    void commit();

    void cancel();

    Xapian::docid add_document(const Xapian::Document & doc);

    void delete_document(Xapian::docid did);
    void delete_document(const std::string & unique_term);

    void replace_document(Xapian::docid did, const Xapian::Document & doc);
    Xapian::docid replace_document(const std::string & unique_term,
				   const Xapian::Document & document);

    std::string get_uuid() const;

    std::string get_metadata(const std::string& key) const;

    void set_metadata(const std::string& key, const std::string& value);

    void add_spelling(const std::string& word, Xapian::termcount freqinc) const;

    Xapian::termcount remove_spelling(const std::string& word,
				      Xapian::termcount freqdec) const;

    int get_backend_info(std::string* path) const {
	if (path) *path = context;
	return BACKEND_REMOTE;
    }

    bool locked() const;

    std::string get_description() const;
};

#endif // XAPIAN_INCLUDED_REMOTE_DATABASE_H
