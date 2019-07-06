#include <stdio.h>


int Add();
int Sub();
int main(){
printf("Welcome to Calculator\n\n");
Add();
return 0;

}


int a,b;
int Add(){
printf("Enter the Two digit !!\n");
scanf("%d%d",&a,&b);
printf("\nAdd Result %d+ %d = %d\n",a,b,a+b);
printf("!ctrl + l");
clrscr();
Sub();
return 0;
}

void clrscr()
{
    system("@cls||clear");
}

int Mul();
int Sub(){
printf("Enter the Two digit !!\n");
scanf("%d%d",&a,&b);
printf("\nSub Result %d - %d = %d\n",a,b,a-b);
Mul();
return 0;
}


int Div();
int Mul(){
printf("Enter the Two digit !!\n");
scanf("%d%d",&a,&b);
printf("\nMul Result %d * %d = %d\n",a,b,a*b);
Div();
return 0;
}




int Div(){
printf("Enter the Two digit !!\n");
scanf("%d%d",&a,&b);
printf("\nDiv Result %d / %d = %d\n",a,b,a/b);
return 0;
}









