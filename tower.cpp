#include <iostream>
using namespace std;
class stack
{
    int a[10], top;

public:
    int n;
    char id;
    stack()
    {
        static int stk = 1;
        top = -1;
        cout << "enter name of stack" << stk++ << ":";
        cin >> id;
    }
    void get();
    void push(int x);
    int pop();
    void display();
};
void stack::get()
{
    cout << "enter no of rings";
    cin >> n;
    if (n > 10)
    {
        cout << "too many disks!!max is 10\n";
        return;
    }
    for (int i = 0; i < n; i++)
    {
        a[i] = i + 1;
    }
    top = n - 1;
}
void stack::push(int x)
{
    if (top >= 9)
    {
        cout << "stack overflow";
        return;
    }
    a[++top] = x;
}
int stack::pop()
{
    if (top < 0)
    {
        cout << "stack underflow";
        return -1;
    }
    return a[top--];
}
void stack::display()
{
    if (top > -1)
    {
        cout << "[";
        for (int i = top; i >= 0; i--)
        {
            if (i == 0)
                cout << a[i];
            else
                cout << a[i] << ",";
        }
        cout << "]";
    }
    else
        cout << "[empty\n]";
}
void toh(int n, stack *from, stack *to, stack *via)
{
    if (n > 0)
    {
        toh(n - 1, from, via, to);
        int p = from->pop();
        if (p != -1)
        {
            to->push(p);
            cout << "moving disk" << p << "from" << from->id << "to" << to->id << endl;
        }
        cout << "stack" << from->id << ":";
        from->display();
        cout << "stack" << to->id << ":";
        to->display();
        cout << "stack" << via->id << ":";
        via->display();
        toh(n - 1, via, to, from);
    }
}
int main()
{
    stack a, b, c;
    a.get();
    cout << "disks in stack" << a.id << "\n";
    a.display();
    int y;
    cout << "to which stack do u want to move all disks(2or3)?";
    cin >> y;
    if (y == 3)
        toh(a.n, &a, &c, &b);
    else if (y == 2)
        toh(a.n, &a, &b, &c);
    else
        cout << "invalid selection";
    return 0;
}