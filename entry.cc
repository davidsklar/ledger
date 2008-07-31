/*
 * Copyright (c) 2003-2008, John Wiegley.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 * - Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 *
 * - Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in the
 *   documentation and/or other materials provided with the distribution.
 *
 * - Neither the name of New Artisans LLC nor the names of its
 *   contributors may be used to endorse or promote products derived from
 *   this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "entry.h"
#include "journal.h"
#include "format.h"

namespace ledger {

entry_base_t::entry_base_t(const entry_base_t& e)
  : supports_flags<>(), journal(NULL),
    beg_pos(0), beg_line(0), end_pos(0), end_line(0)
{
  TRACE_CTOR(entry_base_t, "copy");

  for (xacts_list::const_iterator i = e.xacts.begin();
       i != e.xacts.end();
       i++)
    xacts.push_back(new xact_t(**i));
}

entry_base_t::~entry_base_t()
{
  TRACE_DTOR(entry_base_t);

  for (xacts_list::iterator i = xacts.begin();
       i != xacts.end();
       i++)
    // If the transaction is a temporary, it will be destructed when the
    // temporary is.  If it's from a binary cache, we can safely destruct it
    // but its memory will be deallocated with the cache.
    if (! (*i)->has_flags(XACT_TEMP)) {
      if (! (*i)->has_flags(XACT_IN_CACHE))
	checked_delete(*i);
      else
	(*i)->~xact_t();
    }
}

void entry_base_t::add_xact(xact_t * xact)
{
  xacts.push_back(xact);
}

bool entry_base_t::remove_xact(xact_t * xact)
{
  xacts.remove(xact);
  return true;
}

bool entry_base_t::finalize()
{
  // Scan through and compute the total balance for the entry.  This is used
  // for auto-calculating the value of entries with no cost, and the per-unit
  // price of unpriced commodities.

  // (let ((balance 0)
  //       null-xact)

  value_t	  balance;
  xact_t * null_xact = NULL;

  //   (do-xacts (xact entry)
  //     (when (xact-must-balance-p xact)
  //       (let ((amt (xact-amount* xact)))
  //         (if amt
  //             (setf balance (add balance (or (xact-cost xact) amt)))
  //             (if null-xact
  //                 (error "Only one xact with null amount allowed ~
  //                         per entry (beg ~S end ~S)"
  //                        (item-position-begin-line (entry-position entry))
  //                        (item-position-end-line (entry-position entry)))
  //                 (setf null-xact xact))))))
  //

  for (xacts_list::const_iterator x = xacts.begin();
       x != xacts.end();
       x++) {
    if ((*x)->must_balance()) {
      amount_t& p((*x)->cost ? *(*x)->cost : (*x)->amount);
      if (! p.is_null()) {
	if (balance.is_null())
	  balance = p;
	else
	  balance += p;
      } else {
	if (null_xact)
	  throw_(std::logic_error,
		 "Only one xact with null amount allowed per entry");
	else
	  null_xact = *x;
      }
    }
  }
  assert(balance.valid());

  DEBUG("ledger.journal.finalize", "initial balance = " << balance);

  // If there is only one xact, balance against the default account if
  // one has been set.

  //   (when (= 1 (length (entry-xacts entry)))
  //     (if-let ((default-account
  //                  (journal-default-account (entry-journal entry))))
  //       (setf null-xact
  //             (make-xact :entry entry
  //                               :status (xact-status
  //                                        (first (entry-xacts entry)))
  //                               :account default-account
  //                               :generatedp t))
  //       (add-xact entry null-xact)))

  if (journal && journal->basket && xacts.size() == 1) {
    // jww (2008-07-24): Need to make the rest of the code aware of what to do
    // when it sees a generated xact.
    null_xact = new xact_t(journal->basket, XACT_GENERATED);
    null_xact->state = (*xacts.begin())->state;
    add_xact(null_xact);
  }

  if (null_xact != NULL) {
    // If one xact has no value at all, its value will become the
    // inverse of the rest.  If multiple commodities are involved, multiple
    // xacts are generated to balance them all.

    // (progn
    //   (if (balance-p balance)
    //       (let ((first t))
    //         (dolist (amount (balance-amounts balance))
    //           (if first
    //               (setf (xact-amount* null-xact) (negate amount)
    //                     first nil)
    //               (add-xact
    //                entry
    //                (make-xact :entry entry
    //                                  :account (xact-account null-xact)
    //                                  :amount (negate amount)
    //                                  :generatedp t)))))
    //       (setf (xact-amount* null-xact) (negate balance)
    //             (xact-calculatedp null-xact) t))
    //
    //   (setf balance 0))

    if (balance.is_balance()) {
      bool first = true;
      const balance_t& bal(balance.as_balance());
      for (balance_t::amounts_map::const_iterator i = bal.amounts.begin();
	   i != bal.amounts.end();
	   i++) {
	if (first) {
	  null_xact->amount = (*i).second.negate();
	  first = false;
	} else {
	  add_xact(new xact_t(null_xact->account,
					    (*i).second.negate(),
					    XACT_GENERATED));
	}
      }
    } else {
      null_xact->amount = balance.as_amount().negate();
      null_xact->add_flags(XACT_CALCULATED);
    }
    balance = NULL_VALUE;

  }
  else if (balance.is_balance() &&
	   balance.as_balance().amounts.size() == 2) {
    // When an entry involves two different commodities (regardless of how
    // many xacts there are) determine the conversion ratio by dividing
    // the total value of one commodity by the total value of the other.  This
    // establishes the per-unit cost for this xact for both
    // commodities.

    // (when (and (balance-p balance)
    //            (= 2 (balance-commodity-count balance)))
    //   (destructuring-bind (x y) (balance-amounts balance)
    //     (let ((a-commodity (amount-commodity x))
    //           (per-unit-cost (value-abs (divide x y))))
    //       (do-xacts (xact entry)
    //         (let ((amount (xact-amount* xact)))
    //           (unless (or (xact-cost xact)
    //                       (not (xact-must-balance-p xact))
    //                       (commodity-equal (amount-commodity amount)
    //                                        a-commodity))
    //             (setf balance (subtract balance amount)
    //                   (xact-cost xact) (multiply per-unit-cost amount)
    //                   balance (add balance (xact-cost xact))))))))))

    const balance_t& bal(balance.as_balance());

    balance_t::amounts_map::const_iterator a = bal.amounts.begin();
    
    const amount_t& x((*a++).second);
    const amount_t& y((*a++).second);

    if (! y.is_realzero()) {
      amount_t per_unit_cost = (x / y).abs();

      commodity_t& comm(x.commodity());

      for (xacts_list::const_iterator x = xacts.begin();
	   x != xacts.end();
	   x++) {
	const amount_t& x_amt((*x)->amount);

	if (! ((*x)->cost ||
	       ! (*x)->must_balance() ||
	       x_amt.commodity() == comm)) {
	  DEBUG("ledger.journal.finalize", "before operation 1 = " << balance);
	  balance -= x_amt;
	  DEBUG("ledger.journal.finalize", "after operation 1 = " << balance);
	  DEBUG("ledger.journal.finalize", "x_amt = " << x_amt);
	  DEBUG("ledger.journal.finalize", "per_unit_cost = " << per_unit_cost);

	  (*x)->cost = per_unit_cost * x_amt;
	  DEBUG("ledger.journal.finalize", "*(*x)->cost = " << *(*x)->cost);

	  balance += *(*x)->cost;
	  DEBUG("ledger.journal.finalize", "after operation 2 = " << balance);
	}

      }
    }

    DEBUG("ledger.journal.finalize", "resolved balance = " << balance);
  }

  // Now that the xact list has its final form, calculate the balance
  // once more in terms of total cost, accounting for any possible gain/loss
  // amounts.

  // (do-xacts (xact entry)
  //   (when (xact-cost xact)
  //     (let ((amount (xact-amount* xact)))
  //       (assert (not (commodity-equal (amount-commodity amount)
  //                                     (amount-commodity (xact-cost xact)))))
  //       (multiple-value-bind (annotated-amount total-cost basis-cost)
  //           (exchange-commodity amount :total-cost (xact-cost xact)
  //                               :moment (entry-date entry)
  //                               :tag (entry-code entry))
  //         (if (annotated-commodity-p (amount-commodity amount))
  //             (if-let ((price (annotation-price
  //                              (commodity-annotation
  //                               (amount-commodity amount)))))
  //               (setf balance
  //                     (add balance (subtract basis-cost total-cost))))
  //             (setf (xact-amount* xact) annotated-amount))))))

  for (xacts_list::const_iterator x = xacts.begin();
       x != xacts.end();
       x++) {
    if ((*x)->cost) {
      const amount_t& x_amt((*x)->amount);

      assert(x_amt.commodity() != (*x)->cost->commodity());

      entry_t * entry = dynamic_cast<entry_t *>(this);

      // jww (2008-07-24): Pass the entry's code here if we can, as the
      // auto-tag
      amount_t final_cost;
      amount_t basis_cost;
      amount_t ann_amount =
	commodity_t::exchange(x_amt, final_cost, basis_cost,
			      (*x)->cost, none, (*x)->actual_date(),
			      entry ? entry->code : optional<string>());

      if ((*x)->amount.commodity_annotated()) {
	if (ann_amount.annotation_details().price) {
	  if (balance.is_null())
	    balance = basis_cost - final_cost;
	  else
	    balance += basis_cost - final_cost;
	}
      } else {
	(*x)->amount = ann_amount;
      }
    }
  }

  DEBUG("ledger.journal.finalize", "final balance = " << balance);

  // (if (value-zerop balance)
  //     (prog1
  //         entry
  //       (setf (entry-normalizedp entry) t))
  //     (error "Entry does not balance (beg ~S end ~S); remaining balance is:~%~A"
  //            (item-position-begin-line (entry-position entry))
  //            (item-position-end-line (entry-position entry))
  //            (format-value balance :width 20)))

  if (! balance.is_null()) {
    balance.round();
    if (! balance.is_zero()) {
      error * err =
	new balance_error("Entry does not balance",
			  new entry_context(*this, "While balancing entry:"));
      err->context.push_front
	(new value_context(balance, "Unbalanced remainder is:"));
      throw err;
    }
  }

  return true;
}

entry_t::entry_t(const entry_t& e)
  : entry_base_t(e), scope_t(), _date(e._date), _date_eff(e._date_eff),
    code(e.code), payee(e.payee)
{
  TRACE_CTOR(entry_t, "copy");

  for (xacts_list::const_iterator i = xacts.begin();
       i != xacts.end();
       i++)
    (*i)->entry = this;
}

bool entry_t::get_state(xact_t::state_t * state) const
{
  bool first  = true;
  bool hetero = false;

  for (xacts_list::const_iterator i = xacts.begin();
       i != xacts.end();
       i++) {
    if (first) {
      *state = (*i)->state;
      first = false;
    }
    else if (*state != (*i)->state) {
      hetero = true;
      break;
    }
  }

  return ! hetero;
}

void entry_t::add_xact(xact_t * xact)
{
  xact->entry = this;
  entry_base_t::add_xact(xact);
}

namespace {
  value_t get_date(call_scope_t& scope)
  {
    entry_t& entry(downcast<entry_t>(*scope.parent));
    return entry.date();
  }

  value_t get_payee(call_scope_t& scope)
  {
    entry_t& entry(downcast<entry_t>(*scope.parent));
    return value_t(entry.payee, true);
  }
}

expr_t::ptr_op_t entry_t::lookup(const string& name)
{
  switch (name[0]) {
  case 'd':
    if (name[1] == '\0' || name == "date")
      return WRAP_FUNCTOR(bind(get_date, _1));
    break;
  case 'p':
    if (name[1] == '\0' || name == "payee")
      return WRAP_FUNCTOR(bind(get_payee, _1));
    break;
  }

#if 0
  // jww (2008-07-29): Should it go to the containing journal next, or to the
  // session?
  return entry->lookup(name);
#else
  return expr_t::ptr_op_t();
#endif
}

bool entry_t::valid() const
{
  if (! is_valid(_date) || ! journal) {
    DEBUG("ledger.validate", "entry_t: ! _date || ! journal");
    return false;
  }

  for (xacts_list::const_iterator i = xacts.begin();
       i != xacts.end();
       i++)
    if ((*i)->entry != this || ! (*i)->valid()) {
      DEBUG("ledger.validate", "entry_t: xact not valid");
      return false;
    }

  return true;
}

void entry_context::describe(std::ostream& out) const throw()
{
  if (! desc.empty())
    out << desc << std::endl;

  print_entry(out, entry, "  ");
}

void auto_entry_t::extend_entry(entry_base_t& entry, bool post)
{
  xacts_list initial_xacts(entry.xacts.begin(),
				  entry.xacts.end());

  for (xacts_list::iterator i = initial_xacts.begin();
       i != initial_xacts.end();
       i++) {
    if (predicate(**i)) {
      for (xacts_list::iterator t = xacts.begin();
	   t != xacts.end();
	   t++) {
	amount_t amt;
	assert((*t)->amount);
	if (! (*t)->amount.commodity()) {
	  if (! post)
	    continue;
	  assert((*i)->amount);
	  amt = (*i)->amount * (*t)->amount;
	} else {
	  if (post)
	    continue;
	  amt = (*t)->amount;
	}

	account_t * account  = (*t)->account;
	string fullname = account->fullname();
	assert(! fullname.empty());
	if (fullname == "$account" || fullname == "@account")
	  account = (*i)->account;

	xact_t * xact
	  = new xact_t(account, amt, (*t)->flags() | XACT_AUTO);

	// Copy over details so that the resulting xact is a mirror of
	// the automated entry's one.
	xact->state	= (*t)->state;
	xact->_date	= (*t)->_date;
	xact->_date_eff = (*t)->_date_eff;
	xact->note	= (*t)->note;
	xact->beg_pos	= (*t)->beg_pos;
	xact->beg_line	= (*t)->beg_line;
	xact->end_pos	= (*t)->end_pos;
	xact->end_line	= (*t)->end_line;

	entry.add_xact(xact);
      }
    }
  }
}

void extend_entry_base(journal_t * journal, entry_base_t& entry, bool post)
{
  for (auto_entries_list::iterator i = journal->auto_entries.begin();
       i != journal->auto_entries.end();
       i++)
    (*i)->extend_entry(entry, post);
}

} // namespace ledger