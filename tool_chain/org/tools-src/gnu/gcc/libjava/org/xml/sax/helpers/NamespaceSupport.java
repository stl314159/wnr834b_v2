// NamespaceSupport.java - generic Namespace support for SAX.
// Written by David Megginson, sax@megginson.com
// This class is in the Public Domain.  NO WARRANTY!

// $Id: NamespaceSupport.java,v 1.1.1.1 2010/03/05 07:30:16 reynolds Exp $

package org.xml.sax.helpers;

import java.util.EmptyStackException;
import java.util.Enumeration;
import java.util.Hashtable;
import java.util.Vector;


/**
 * Encapsulate Namespace logic for use by SAX drivers.
 *
 * <blockquote>
 * <em>This module, both source code and documentation, is in the
 * Public Domain, and comes with <strong>NO WARRANTY</strong>.</em>
 * </blockquote>
 *
 * <p>This class encapsulates the logic of Namespace processing:
 * it tracks the declarations currently in force for each context
 * and automatically processes qualified XML 1.0 names into their
 * Namespace parts; it can also be used in reverse for generating
 * XML 1.0 from Namespaces.</p>
 *
 * <p>Namespace support objects are reusable, but the reset method
 * must be invoked between each session.</p>
 *
 * <p>Here is a simple session:</p>
 *
 * <pre>
 * String parts[] = new String[3];
 * NamespaceSupport support = new NamespaceSupport();
 *
 * support.pushContext();
 * support.declarePrefix("", "http://www.w3.org/1999/xhtml");
 * support.declarePrefix("dc", "http://www.purl.org/dc#");
 *
 * String parts[] = support.processName("p", parts, false);
 * System.out.println("Namespace URI: " + parts[0]);
 * System.out.println("Local name: " + parts[1]);
 * System.out.println("Raw name: " + parts[2]);

 * String parts[] = support.processName("dc:title", parts, false);
 * System.out.println("Namespace URI: " + parts[0]);
 * System.out.println("Local name: " + parts[1]);
 * System.out.println("Raw name: " + parts[2]);

 * support.popContext();
 * </pre>
 *
 * <p>Note that this class is optimized for the use case where most
 * elements do not contain Namespace declarations: if the same
 * prefix/URI mapping is repeated for each context (for example), this
 * class will be somewhat less efficient.</p>
 *
 * @since SAX 2.0
 * @author David Megginson, 
 *         <a href="mailto:sax@megginson.com">sax@megginson.com</a>
 * @version 2.0
 */
public class NamespaceSupport
{


    ////////////////////////////////////////////////////////////////////
    // Constants.
    ////////////////////////////////////////////////////////////////////


    /**
     * The XML Namespace as a constant.
     *
     * <p>This is the Namespace URI that is automatically mapped
     * to the "xml" prefix.</p>
     */
    public final static String XMLNS =
	"http://www.w3.org/XML/1998/namespace";


    /**
     * An empty enumeration.
     */
    private final static Enumeration EMPTY_ENUMERATION =
	new Vector().elements();


    ////////////////////////////////////////////////////////////////////
    // Constructor.
    ////////////////////////////////////////////////////////////////////


    /**
     * Create a new Namespace support object.
     */
    public NamespaceSupport ()
    {
	reset();
    }



    ////////////////////////////////////////////////////////////////////
    // Context management.
    ////////////////////////////////////////////////////////////////////


    /**
     * Reset this Namespace support object for reuse.
     *
     * <p>It is necessary to invoke this method before reusing the
     * Namespace support object for a new session.</p>
     */
    public void reset ()
    {
	contexts = new Context[32];
	contextPos = 0;
	contexts[contextPos] = currentContext = new Context();
	currentContext.declarePrefix("xml", XMLNS);
    }


    /**
     * Start a new Namespace context.
     *
     * <p>Normally, you should push a new context at the beginning
     * of each XML element: the new context will automatically inherit
     * the declarations of its parent context, but it will also keep
     * track of which declarations were made within this context.</p>
     *
     * <p>The Namespace support object always starts with a base context
     * already in force: in this context, only the "xml" prefix is
     * declared.</p>
     *
     * @see #popContext
     */
    public void pushContext ()
    {
	int max = contexts.length;
	contextPos++;

				// Extend the array if necessary
	if (contextPos >= max) {
	    Context newContexts[] = new Context[max*2];
	    System.arraycopy(contexts, 0, newContexts, 0, max);
	    max *= 2;
	    contexts = newContexts;
	}

				// Allocate the context if necessary.
	currentContext = contexts[contextPos];
	if (currentContext == null) {
	    contexts[contextPos] = currentContext = new Context();
	}

				// Set the parent, if any.
	if (contextPos > 0) {
	    currentContext.setParent(contexts[contextPos - 1]);
	}
    }


    /**
     * Revert to the previous Namespace context.
     *
     * <p>Normally, you should pop the context at the end of each
     * XML element.  After popping the context, all Namespace prefix
     * mappings that were previously in force are restored.</p>
     *
     * <p>You must not attempt to declare additional Namespace
     * prefixes after popping a context, unless you push another
     * context first.</p>
     *
     * @see #pushContext
     */
    public void popContext ()
    {
	contextPos--;
	if (contextPos < 0) {
	    throw new EmptyStackException();
	}
	currentContext = contexts[contextPos];
    }



    ////////////////////////////////////////////////////////////////////
    // Operations within a context.
    ////////////////////////////////////////////////////////////////////


    /**
     * Declare a Namespace prefix.
     *
     * <p>This method declares a prefix in the current Namespace
     * context; the prefix will remain in force until this context
     * is popped, unless it is shadowed in a descendant context.</p>
     *
     * <p>To declare a default Namespace, use the empty string.  The
     * prefix must not be "xml" or "xmlns".</p>
     *
     * <p>Note that you must <em>not</em> declare a prefix after
     * you've pushed and popped another Namespace.</p>
     *
     * <p>Note that there is an asymmetry in this library: while {@link
     * #getPrefix getPrefix} will not return the default "" prefix,
     * even if you have declared one; to check for a default prefix,
     * you have to look it up explicitly using {@link #getURI getURI}.
     * This asymmetry exists to make it easier to look up prefixes
     * for attribute names, where the default prefix is not allowed.</p>
     *
     * @param prefix The prefix to declare, or null for the empty
     *        string.
     * @param uri The Namespace URI to associate with the prefix.
     * @return true if the prefix was legal, false otherwise
     * @see #processName
     * @see #getURI
     * @see #getPrefix
     */
    public boolean declarePrefix (String prefix, String uri)
    {
	if (prefix.equals("xml") || prefix.equals("xmlns")) {
	    return false;
	} else {
	    currentContext.declarePrefix(prefix, uri);
	    return true;
	}
    }


    /**
     * Process a raw XML 1.0 name.
     *
     * <p>This method processes a raw XML 1.0 name in the current
     * context by removing the prefix and looking it up among the
     * prefixes currently declared.  The return value will be the
     * array supplied by the caller, filled in as follows:</p>
     *
     * <dl>
     * <dt>parts[0]</dt>
     * <dd>The Namespace URI, or an empty string if none is
     *  in use.</dd>
     * <dt>parts[1]</dt>
     * <dd>The local name (without prefix).</dd>
     * <dt>parts[2]</dt>
     * <dd>The original raw name.</dd>
     * </dl>
     *
     * <p>All of the strings in the array will be internalized.  If
     * the raw name has a prefix that has not been declared, then
     * the return value will be null.</p>
     *
     * <p>Note that attribute names are processed differently than
     * element names: an unprefixed element name will received the
     * default Namespace (if any), while an unprefixed element name
     * will not.</p>
     *
     * @param qName The raw XML 1.0 name to be processed.
     * @param parts An array supplied by the caller, capable of
     *        holding at least three members.
     * @param isAttribute A flag indicating whether this is an
     *        attribute name (true) or an element name (false).
     * @return The supplied array holding three internalized strings 
     *        representing the Namespace URI (or empty string), the
     *        local name, and the raw XML 1.0 name; or null if there
     *        is an undeclared prefix.
     * @see #declarePrefix
     * @see java.lang.String#intern */
    public String [] processName (String qName, String parts[],
				  boolean isAttribute)
    {
	String myParts[] = currentContext.processName(qName, isAttribute);
	if (myParts == null) {
	    return null;
	} else {
	    parts[0] = myParts[0];
	    parts[1] = myParts[1];
	    parts[2] = myParts[2];
	    return parts;
	}
    }


    /**
     * Look up a prefix and get the currently-mapped Namespace URI.
     *
     * <p>This method looks up the prefix in the current context.
     * Use the empty string ("") for the default Namespace.</p>
     *
     * @param prefix The prefix to look up.
     * @return The associated Namespace URI, or null if the prefix
     *         is undeclared in this context.
     * @see #getPrefix
     * @see #getPrefixes
     */
    public String getURI (String prefix)
    {
	return currentContext.getURI(prefix);
    }


    /**
     * Return an enumeration of all prefixes currently declared.
     *
     * <p><strong>Note:</strong> if there is a default prefix, it will not be
     * returned in this enumeration; check for the default prefix
     * using the {@link #getURI getURI} with an argument of "".</p>
     *
     * @return An enumeration of all prefixes declared in the
     *         current context except for the empty (default)
     *         prefix.
     * @see #getDeclaredPrefixes
     * @see #getURI
     */
    public Enumeration getPrefixes ()
    {
	return currentContext.getPrefixes();
    }


    /**
     * Return one of the prefixes mapped to a Namespace URI.
     *
     * <p>If more than one prefix is currently mapped to the same
     * URI, this method will make an arbitrary selection; if you
     * want all of the prefixes, use the {@link #getPrefixes}
     * method instead.</p>
     *
     * <p><strong>Note:</strong> this will never return the empty (default) prefix;
     * to check for a default prefix, use the {@link #getURI getURI}
     * method with an argument of "".</p>
     *
     * @param uri The Namespace URI.
     * @param isAttribute true if this prefix is for an attribute
     *        (and the default Namespace is not allowed).
     * @return One of the prefixes currently mapped to the URI supplied,
     *         or null if none is mapped or if the URI is assigned to
     *         the default Namespace.
     * @see #getPrefixes(java.lang.String)
     * @see #getURI
     */
    public String getPrefix (String uri)
    {
	return currentContext.getPrefix(uri);
    }


    /**
     * Return an enumeration of all prefixes currently declared for a URI.
     *
     * <p>This method returns prefixes mapped to a specific Namespace
     * URI.  The xml: prefix will be included.  If you want only one
     * prefix that's mapped to the Namespace URI, and you don't care 
     * which one you get, use the {@link #getPrefix getPrefix}
     *  method instead.</p>
     *
     * <p><strong>Note:</strong> the empty (default) prefix is <em>never</em> included
     * in this enumeration; to check for the presence of a default
     * Namespace, use the {@link #getURI getURI} method with an
     * argument of "".</p>
     *
     * @param uri The Namespace URI.
     * @return An enumeration of all prefixes declared in the
     *         current context.
     * @see #getPrefix
     * @see #getDeclaredPrefixes
     * @see #getURI
     */
    public Enumeration getPrefixes (String uri)
    {
	Vector prefixes = new Vector();
	Enumeration allPrefixes = getPrefixes();
	while (allPrefixes.hasMoreElements()) {
	    String prefix = (String)allPrefixes.nextElement();
	    if (uri.equals(getURI(prefix))) {
		prefixes.addElement(prefix);
	    }
	}
	return prefixes.elements();
    }


    /**
     * Return an enumeration of all prefixes declared in this context.
     *
     * <p>The empty (default) prefix will be included in this 
     * enumeration; note that this behaviour differs from that of
     * {@link #getPrefix} and {@link #getPrefixes}.</p>
     *
     * @return An enumeration of all prefixes declared in this
     *         context.
     * @see #getPrefixes
     * @see #getURI
     */
    public Enumeration getDeclaredPrefixes ()
    {
	return currentContext.getDeclaredPrefixes();
    }



    ////////////////////////////////////////////////////////////////////
    // Internal state.
    ////////////////////////////////////////////////////////////////////

    private Context contexts[];
    private Context currentContext;
    private int contextPos;



    ////////////////////////////////////////////////////////////////////
    // Internal classes.
    ////////////////////////////////////////////////////////////////////

    /**
     * Internal class for a single Namespace context.
     *
     * <p>This module caches and reuses Namespace contexts, so the number allocated
     * will be equal to the element depth of the document, not to the total
     * number of elements (i.e. 5-10 rather than tens of thousands).</p>
     */
    final class Context {

	/**
	 * Create the root-level Namespace context.
	 */
	Context ()
	{
	    copyTables();
	}
	
	
	/**
	 * (Re)set the parent of this Namespace context.
	 *
	 * @param context The parent Namespace context object.
	 */
	void setParent (Context parent)
	{
	    this.parent = parent;
	    declarations = null;
	    prefixTable = parent.prefixTable;
	    uriTable = parent.uriTable;
	    elementNameTable = parent.elementNameTable;
	    attributeNameTable = parent.attributeNameTable;
	    defaultNS = parent.defaultNS;
	    tablesDirty = false;
	}
	
	
	/**
	 * Declare a Namespace prefix for this context.
	 *
	 * @param prefix The prefix to declare.
	 * @param uri The associated Namespace URI.
	 * @see org.xml.sax.helpers.NamespaceSupport#declarePrefix
	 */
	void declarePrefix (String prefix, String uri)
	{
				// Lazy processing...
	    if (!tablesDirty) {
		copyTables();
	    }
	    if (declarations == null) {
		declarations = new Vector();
	    }
	    
	    prefix = prefix.intern();
	    uri = uri.intern();
	    if ("".equals(prefix)) {
		if ("".equals(uri)) {
		    defaultNS = null;
		} else {
		    defaultNS = uri;
		}
	    } else {
		prefixTable.put(prefix, uri);
		uriTable.put(uri, prefix); // may wipe out another prefix
	    }
	    declarations.addElement(prefix);
	}


	/**
	 * Process a raw XML 1.0 name in this context.
	 *
	 * @param qName The raw XML 1.0 name.
	 * @param isAttribute true if this is an attribute name.
	 * @return An array of three strings containing the
	 *         URI part (or empty string), the local part,
	 *         and the raw name, all internalized, or null
	 *         if there is an undeclared prefix.
	 * @see org.xml.sax.helpers.NamespaceSupport#processName
	 */
	String [] processName (String qName, boolean isAttribute)
	{
	    String name[];
	    Hashtable table;
	    
				// Select the appropriate table.
	    if (isAttribute) {
		table = elementNameTable;
	    } else {
		table = attributeNameTable;
	    }
	    
				// Start by looking in the cache, and
				// return immediately if the name
				// is already known in this content
	    name = (String[])table.get(qName);
	    if (name != null) {
		return name;
	    }
	    
				// We haven't seen this name in this
				// context before.
	    name = new String[3];
	    int index = qName.indexOf(':');
	    
	    
				// No prefix.
	    if (index == -1) {
		if (isAttribute || defaultNS == null) {
		    name[0] = "";
		} else {
		    name[0] = defaultNS;
		}
		name[1] = qName.intern();
		name[2] = name[1];
	    }
	    
				// Prefix
	    else {
		String prefix = qName.substring(0, index);
		String local = qName.substring(index+1);
		String uri;
		if ("".equals(prefix)) {
		    uri = defaultNS;
		} else {
		    uri = (String)prefixTable.get(prefix);
		}
		if (uri == null) {
		    return null;
		}
		name[0] = uri;
		name[1] = local.intern();
		name[2] = qName.intern();
	    }
	    
				// Save in the cache for future use.
	    table.put(name[2], name);
	    tablesDirty = true;
	    return name;
	}
	

	/**
	 * Look up the URI associated with a prefix in this context.
	 *
	 * @param prefix The prefix to look up.
	 * @return The associated Namespace URI, or null if none is
	 *         declared.	
	 * @see org.xml.sax.helpers.NamespaceSupport#getURI
	 */
	String getURI (String prefix)
	{
	    if ("".equals(prefix)) {
		return defaultNS;
	    } else if (prefixTable == null) {
		return null;
	    } else {
		return (String)prefixTable.get(prefix);
	    }
	}


	/**
	 * Look up one of the prefixes associated with a URI in this context.
	 *
	 * <p>Since many prefixes may be mapped to the same URI,
	 * the return value may be unreliable.</p>
	 *
	 * @param uri The URI to look up.
	 * @return The associated prefix, or null if none is declared.
	 * @see org.xml.sax.helpers.NamespaceSupport#getPrefix
	 */
	String getPrefix (String uri)
	{
	    if (uriTable == null) {
		return null;
	    } else {
		return (String)uriTable.get(uri);
	    }
	}
	
	
	/**
	 * Return an enumeration of prefixes declared in this context.
	 *
	 * @return An enumeration of prefixes (possibly empty).
	 * @see org.xml.sax.helpers.NamespaceSupport#getDeclaredPrefixes
	 */
	Enumeration getDeclaredPrefixes ()
	{
	    if (declarations == null) {
		return EMPTY_ENUMERATION;
	    } else {
		return declarations.elements();
	    }
	}
	
	
	/**
	 * Return an enumeration of all prefixes currently in force.
	 *
	 * <p>The default prefix, if in force, is <em>not</em>
	 * returned, and will have to be checked for separately.</p>
	 *
	 * @return An enumeration of prefixes (never empty).
	 * @see org.xml.sax.helpers.NamespaceSupport#getPrefixes
	 */
	Enumeration getPrefixes ()
	{
	    if (prefixTable == null) {
		return EMPTY_ENUMERATION;
	    } else {
		return prefixTable.keys();
	    }
	}
	
	

	////////////////////////////////////////////////////////////////
	// Internal methods.
	////////////////////////////////////////////////////////////////


	/**
	 * Copy on write for the internal tables in this context.
	 *
	 * <p>This class is optimized for the normal case where most
	 * elements do not contain Namespace declarations.</p>
	 */	
	private void copyTables ()
	{
	    if (prefixTable != null) {
		prefixTable = (Hashtable)prefixTable.clone();
	    } else {
		prefixTable = new Hashtable();
	    }
	    if (uriTable != null) {
		uriTable = (Hashtable)uriTable.clone();
	    } else {
		uriTable = new Hashtable();
	    }
	    elementNameTable = new Hashtable();
	    attributeNameTable = new Hashtable();
	    tablesDirty = true;
	}



	////////////////////////////////////////////////////////////////
	// Protected state.
	////////////////////////////////////////////////////////////////
	
	Hashtable prefixTable;
	Hashtable uriTable;
	Hashtable elementNameTable;
	Hashtable attributeNameTable;
	String defaultNS = null;
	


	////////////////////////////////////////////////////////////////
	// Internal state.
	////////////////////////////////////////////////////////////////
	
	private Vector declarations = null;
	private boolean tablesDirty = false;
	private Context parent = null;
    }
}

// end of NamespaceSupport.java
